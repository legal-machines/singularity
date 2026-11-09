#pragma once

#include "Provider/Types/UsageMetadata.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace singularity::research {

// DURABLE PAUSE. A research loop that stops to wait on the lawyer - a question
// only they can answer (lawyer.ask), or a web search or page Ask first puts to
// them - first saves where it stands: the prompt as it is at that moment, what
// the model returned (the question, or the tool call), the loop's counters, the
// external evidence served so far and what the lawyer was shown. If Singularity
// closes while it waits, the question or the search is still in front of the
// lawyer after the next launch; once answered, the resumed run continues from
// this record with the answer, making no model call it already made.
//
// A pause holds no restoration mapping: its prompt is the loop's own text, protected
// again on every call. The protection table its calls were numbered by is named only by
// its random scope id; a resumed attempt numbers a table of its own, afresh, from the
// same local text.
//
// {
//   "version": 1,
//   "kind": "lawyer_question" | "research_approval",
//   "approvalId", "toolId", "intentDigest", "summary", "effect",
//   "question"        the question as the lawyer was shown it (lawyer_question),
//   "change"          what leaves the device, as the card shows it (research_approval),
//   "rawArguments", "canonicalArguments"   the call as the model made it and as
//                     it was approved to leave (research_approval),
//   "providerId", "modelId",
//   "loop": { "prompt", "answersGiven", "maxModelTurns", "progress": {...},
//             "externalEvidence": [...], counters of the loop's own rounds },
//   "result": { "text", "toolCalls": [{"callId", "wireName", "arguments"}] },
//   "usage"           what the loop's calls were billed up to the pause,
//   "attempt": { "pickedDocumentIds": [...],   (research_attempt_facts)
//                "requestPassages"  the request's own passages, in marker order, as
//                                   {documentId, textHash} - never their text; a resume
//                                   takes up the place only while they are unchanged,
//                "usageBefore"      what the operation was billed before this attempt,
//                "projectEvidence"  what inspection found and gave, by passage identity
//                                   (Context/ProjectEvidence Session::kept()),
//                "protection": { "mode", "symbolScope" } },
//   "decision": null | { "allowed": bool, "answers": json,
//                        "forRequest": true   web research allowed for the rest
//                                             of the request (research_approval) }
// }
inline constexpr int kResearchPauseVersion = 1;

struct ResearchPauseStore {
	// Keeps the operation's pause, replacing any earlier one.
	std::function<void(const std::string &projectId, const std::string &operationId, const nlohmann::json &pause)> save;
	// The operation's pause, or nothing.
	std::function<std::optional<nlohmann::json>(const std::string &projectId, const std::string &operationId)> load;
	// Drops the operation's pause.
	std::function<void(const std::string &projectId, const std::string &operationId)> discard;
};

// Empty until an operation owner installs it: a loop driven directly (a test,
// the console) saves nothing and resumes nothing.
inline ResearchPauseStore &research_pause_store() {
	static ResearchPauseStore store;
	return store;
}

// What the attempt running a loop settled before the loop began, which a resume
// must settle the same way: the documents its pick chose (MatterRequestExecution
// records them for its operation as the attempt starts). A pause keeps them
// under "attempt", so the resumed attempt reads the very same documents - their
// passages carry the source markers the model already read - and never pays
// for the pick again. Process memory: dropped when the operation's run ends.
inline std::mutex &research_attempt_facts_mutex() {
	static std::mutex mutex;
	return mutex;
}

inline std::map<std::pair<std::string, std::string>, nlohmann::json> &research_attempt_facts() {
	static std::map<std::pair<std::string, std::string>, nlohmann::json> facts;
	return facts;
}

inline void set_research_attempt_facts(const std::string &projectId, const std::string &operationId,
		const nlohmann::json &facts) {
	if (operationId.empty()) return;
	const std::lock_guard<std::mutex> lock(research_attempt_facts_mutex());
	research_attempt_facts()[{projectId, operationId}] = facts;
}

inline nlohmann::json research_attempt_facts_for(const std::string &projectId, const std::string &operationId) {
	const std::lock_guard<std::mutex> lock(research_attempt_facts_mutex());
	const auto found = research_attempt_facts().find({projectId, operationId});
	return found == research_attempt_facts().end() ? nlohmann::json::object() : found->second;
}

inline void clear_research_attempt_facts(const std::string &projectId, const std::string &operationId) {
	const std::lock_guard<std::mutex> lock(research_attempt_facts_mutex());
	research_attempt_facts().erase({projectId, operationId});
}

// What a pause keeps of a bill: token counts and the provider's own cost
// evidence, each absent when it was never reported - never zero in its place.
inline nlohmann::json pause_usage_to_json(const providers::UsageMetadata &usage) {
	const auto number = [](const std::optional<long long> &value) {
		return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
	};
	const auto text = [](const std::optional<std::string> &value) {
		return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
	};
	return nlohmann::json{{"inputTokens", number(usage.input_tokens)}, {"outputTokens", number(usage.output_tokens)},
		{"totalTokens", number(usage.total_tokens)}, {"costMicros", number(usage.cost_micros)},
		{"costCurrency", text(usage.cost_currency)}, {"costClassification", text(usage.cost_classification)},
		{"costMethodology", text(usage.cost_methodology)}};
}

inline providers::UsageMetadata pause_usage_from_json(const nlohmann::json &value) {
	providers::UsageMetadata usage;
	if (!value.is_object()) return usage;
	const auto number = [&](const char *key, std::optional<long long> &target) {
		if (value.contains(key) && value.at(key).is_number_integer()) target = value.at(key).get<long long>();
	};
	const auto text = [&](const char *key, std::optional<std::string> &target) {
		if (value.contains(key) && value.at(key).is_string()) target = value.at(key).get<std::string>();
	};
	number("inputTokens", usage.input_tokens);
	number("outputTokens", usage.output_tokens);
	number("totalTokens", usage.total_tokens);
	number("costMicros", usage.cost_micros);
	text("costCurrency", usage.cost_currency);
	text("costClassification", usage.cost_classification);
	text("costMethodology", usage.cost_methodology);
	return usage;
}

// Adds `fields` to what the attempt running the operation has settled (a
// passage inspection gave, what was billed before it began), keeping the rest.
inline void merge_research_attempt_facts(const std::string &projectId, const std::string &operationId,
		const nlohmann::json &fields) {
	if (operationId.empty() || !fields.is_object()) return;
	const std::lock_guard<std::mutex> lock(research_attempt_facts_mutex());
	nlohmann::json &facts = research_attempt_facts()[{projectId, operationId}];
	if (!facts.is_object()) facts = nlohmann::json::object();
	for (const auto &item : fields.items()) facts[item.key()] = item.value();
}

// Whether a pause read back from disk has the shape this file describes, field
// by field, so nothing that reads it trips over a value of the wrong type. A
// pause is a file on the lawyer's disk: one damaged, or written by another
// build, is treated as no pause at all - the chat then offers to send the
// request again - never as an error that leaves the chat without a way on.
inline bool is_well_formed_pause(const nlohmann::json &pause) {
	using Json = nlohmann::json;
	const auto optionalOf = [](const Json &object, const char *key, bool (Json::*isType)() const noexcept) {
		return !object.contains(key) || (object.at(key).*isType)();
	};
	const auto allOf = [](const Json &object, std::initializer_list<const char *> keys,
			bool (Json::*isType)() const noexcept) {
		for (const char *key : keys)
			if (object.contains(key) && !(object.at(key).*isType)()) return false;
		return true;
	};
	const auto stringsIn = [](const Json &array) {
		if (!array.is_array()) return false;
		for (const Json &item : array)
			if (!item.is_string()) return false;
		return true;
	};

	if (!pause.is_object()) return false;
	if (!pause.contains("version") || !pause.at("version").is_number_integer()
			|| pause.at("version").get<long long>() != kResearchPauseVersion)
		return false;
	if (!pause.contains("kind") || !pause.at("kind").is_string()) return false;
	const std::string kind = pause.at("kind").get<std::string>();
	if (kind != "lawyer_question" && kind != "research_approval") return false;
	if (!pause.contains("approvalId") || !pause.at("approvalId").is_string()) return false;
	if (!allOf(pause, {"operationId", "projectId", "toolId", "intentDigest", "summary", "effect", "providerId",
			"modelId"}, &Json::is_string))
		return false;
	if (!allOf(pause, {"usage"}, &Json::is_object)) return false;

	/* - - - - - - */

	if (pause.contains("question")) {
		const Json &question = pause.at("question");
		if (!question.is_object() || !optionalOf(question, "questions", &Json::is_array)) return false;
		if (question.contains("questions"))
			for (const Json &item : question.at("questions"))
				if (!item.is_object()) return false;
	}
	if (pause.contains("decision") && !pause.at("decision").is_null()) {
		const Json &decision = pause.at("decision");
		if (!decision.is_object() || !allOf(decision, {"allowed", "forRequest"}, &Json::is_boolean)) return false;
	}

	/* - - - - - - */

	if (pause.contains("loop")) {
		const Json &loop = pause.at("loop");
		if (!loop.is_object() || !allOf(loop, {"prompt", "answersGiven"}, &Json::is_string)) return false;
		if (!allOf(loop, {"maxModelTurns", "localFactRoundsServed", "projectEvidenceRequestsServed",
				"computedStatementCorrectionsUsed"}, &Json::is_number_unsigned))
			return false;
		if (!allOf(loop, {"lawyerQuestionCorrectionUsed", "localFactsClosed", "projectEvidenceClosed",
				"chronologyCorrectionUsed", "untracedFigureCorrectionUsed"}, &Json::is_boolean))
			return false;
		if (!allOf(loop, {"externalEvidence", "projectEvidenceRequestsSeen"}, &Json::is_array)) return false;
		if (loop.contains("externalEvidence"))
			for (const Json &entry : loop.at("externalEvidence"))
				if (!entry.is_object() || !allOf(entry, {"sourceClass", "evidenceId", "url", "retrievedAt", "excerpt",
						"contentHash"}, &Json::is_string))
					return false;
		if (loop.contains("progress")) {
			const Json &progress = loop.at("progress");
			if (!progress.is_object()) return false;
			for (const auto &item : progress.items())
				if (!item.value().is_number_unsigned()) return false;
		}
	}
	if (pause.contains("result")) {
		const Json &result = pause.at("result");
		if (!result.is_object() || !optionalOf(result, "text", &Json::is_string)
				|| !optionalOf(result, "toolCalls", &Json::is_array))
			return false;
		if (result.contains("toolCalls"))
			for (const Json &call : result.at("toolCalls"))
				if (!call.is_object() || !allOf(call, {"callId", "wireName"}, &Json::is_string)) return false;
	}

	/* - - - - - - */

	if (pause.contains("attempt")) {
		const Json &attempt = pause.at("attempt");
		if (!attempt.is_object() || !allOf(attempt, {"usageBefore", "protection"}, &Json::is_object)) return false;
		if (attempt.contains("pickedDocumentIds") && !stringsIn(attempt.at("pickedDocumentIds"))) return false;
		if (attempt.contains("protection") && !allOf(attempt.at("protection"), {"mode", "symbolScope"}, &Json::is_string))
			return false;
		if (attempt.contains("requestPassages")) {
			if (!attempt.at("requestPassages").is_array()) return false;
			for (const Json &passage : attempt.at("requestPassages"))
				if (!passage.is_object() || !allOf(passage, {"documentId", "textHash"}, &Json::is_string)) return false;
		}
		if (attempt.contains("projectEvidence")) {
			const Json &kept = attempt.at("projectEvidence");
			if (!kept.is_object()) return false;
			if (kept.contains("request") && !stringsIn(kept.at("request"))) return false;
			if (kept.contains("announced") && !stringsIn(kept.at("announced"))) return false;
			if (kept.contains("found")) {
				if (!kept.at("found").is_array()) return false;
				for (const Json &entry : kept.at("found"))
					if (!entry.is_object() || !allOf(entry, {"documentId", "chunkId", "textHash"}, &Json::is_string)
							|| !optionalOf(entry, "given", &Json::is_boolean))
						return false;
			}
		}
	}
	return true;
}

// A decided pause: the lawyer answered the question, or approved or declined
// the search, while nothing was waiting in this process.
inline bool is_decided_pause(const nlohmann::json &pause) {
	return is_well_formed_pause(pause) && pause.contains("decision") && pause.at("decision").is_object();
}

} // namespace singularity::research
