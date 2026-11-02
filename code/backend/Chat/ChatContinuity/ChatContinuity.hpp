#pragma once

#include "Context/PromptAssembler/PromptAssembler.hpp"
#include "Context/SourceLabels/SourceLabels.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Matter/Types/Matter.hpp"
#include "PapinianGuard/Types/FactAnchor.hpp"
#include "Provider/Types/TaskError.hpp"
#include "Tool/LawyerQuestion/LawyerQuestion.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace singularity::chat {

// Seconds since 1970 of a UTC stamp as Singularity writes one
// ("2026-09-23T08:02:41Z", a fraction of a second allowed); nullopt for
// anything else. Counted by the civil calendar, so no platform clock is asked.
inline std::optional<long long> utc_seconds(const std::string &stamp) {
	int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
	if (std::sscanf(stamp.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) != 6)
		return std::nullopt;
	if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;
	const long long shifted = year - (month <= 2 ? 1 : 0);
	const long long era = (shifted >= 0 ? shifted : shifted - 399) / 400;
	const long long yearOfEra = shifted - era * 400;
	const long long dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	const long long dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
	const long long days = era * 146097 + dayOfEra - 719468;
	return days * 86400 + hour * 3600 + minute * 60 + second;
}

// How long Singularity itself worked on an operation, read from its own events:
// from the first to the last, less every wait for the lawyer - an approval
// asked for, or a question put to them, until it was answered - and every
// stretch Singularity was not running it at all, from an interruption (Singularity
// closed while it ran) until the operation was resumed. Neither is Singularity's
// work. nullopt where the events cannot say.
inline std::optional<long long> worked_milliseconds(const matters::MatterTask &task) {
	if (task.operation_events.size() < 2) return std::nullopt;
	long long worked = 0;
	bool waiting = false;
	bool paused = false;
	std::optional<long long> previous;
	for (const matters::AgentOperationEvent &event : task.operation_events) {
		const std::optional<long long> at = utc_seconds(event.created_at);
		if (!at) return std::nullopt;
		if (previous && !waiting && !paused && *at > *previous) worked += *at - *previous;
		previous = at;
		if (event.type == "approval.requested" || event.type == "question.asked") waiting = true;
		else if (event.type == "approval.granted" || event.type == "approval.denied"
				|| event.type == "question.answered" || event.type == "question.left"
				// An approval the running attempt waited for in this process is
				// answered by recording the work as going on, never by an
				// approval event of its own (record_approval_response's live path).
				|| event.type == "operation.working")
			waiting = false;
		else if (event.type == "operation.interrupted") paused = true;
		else if (event.type == "operation.resumed") {
			// A resumed run works again, whatever it was doing when it stopped:
			// a wait for an approval it will ask for anew is not still running.
			paused = false;
			waiting = false;
		}
	}
	return worked * 1000;
}

// Whether the operation ever waited for the lawyer's decision.
inline bool waited_for_approval(const matters::MatterTask &task) {
	return std::any_of(task.operation_events.begin(), task.operation_events.end(),
		[](const matters::AgentOperationEvent &event) { return event.type == "approval.requested"; });
}

// Whether the operation ever put a question to the lawyer. Its run waited,
// still running, until they answered.
inline bool asked_the_lawyer(const matters::MatterTask &task) {
	return std::any_of(task.operation_events.begin(), task.operation_events.end(),
		[](const matters::AgentOperationEvent &event) { return event.type == "question.asked"; });
}

// How much of a turn's request and answer its compact record quotes.
inline constexpr std::size_t kTurnRecordRequestBytes = 1500;
inline constexpr std::size_t kTurnRecordAnswerOpeningBytes = 500;
inline constexpr std::size_t kTurnRecordListLimit = 8;
inline constexpr std::size_t kBriefTurnRecordRequestBytes = 240;
inline constexpr std::size_t kBriefTurnRecordListLimit = 3;
// How many of a compacted conversation's newest turns keep their full record.
// A follow-up builds on the last few turns; everything older needs to be named,
// not re-read. Three is what a person means by "what we were just doing".
inline constexpr std::size_t kCompactionDetailedTurns = 3;

// Where an opening of `text` that ends at `end` must end instead, so that it
// never cuts an operand ⟦…⟧ or a statement {{…}} - or an {{if}} before its
// {{end}} - in two: the arithmetic of an earlier answer travels whole or not
// at all. A half operand is a protected value no longer marked as one, and a
// half statement is words the next request cannot read.
inline std::size_t whole_statements_end(const std::string &text, std::size_t end) {
	static const std::string open = "\xE2\x9F\xA6";   // ⟦
	static const std::string close = "\xE2\x9F\xA7";  // ⟧
	std::size_t cut = end;
	std::size_t operand = std::string::npos;
	std::vector<std::size_t> heads;
	std::vector<std::size_t> conditions;
	for (std::size_t at = 0; at < end;) {
		if (text.compare(at, open.size(), open) == 0) {
			if (operand == std::string::npos) operand = at;
			at += open.size();
		} else if (text.compare(at, close.size(), close) == 0) {
			operand = std::string::npos;
			at += close.size();
		} else if (operand != std::string::npos) {
			++at;
		} else if (text.compare(at, 2, "{{") == 0) {
			heads.push_back(at);
			if (text.compare(at, 5, "{{if ") == 0) conditions.push_back(at);
			else if (text.compare(at, 7, "{{end}}") == 0 && !conditions.empty()) conditions.pop_back();
			at += 2;
		} else if (text.compare(at, 2, "}}") == 0) {
			if (!heads.empty()) heads.pop_back();
			at += 2;
		} else {
			++at;
		}
	}
	if (operand != std::string::npos) cut = std::min(cut, operand);
	if (!heads.empty()) cut = std::min(cut, heads.front());
	if (!conditions.empty()) cut = std::min(cut, conditions.front());
	return cut;
}

// The beginning of `text` within `maximum_bytes`: whole UTF-8 characters,
// ending at a nearby word boundary when there is one, and never inside an
// operand or a statement (whole_statements_end).
inline std::string text_opening(const std::string &text, std::size_t maximum_bytes) {
	if (text.size() <= maximum_bytes) return text;
	std::size_t end = maximum_bytes;
	while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
	const std::size_t space = text.find_last_of(" \t\r\n", end);
	if (space != std::string::npos && space + 120 >= end) end = space;
	end = whole_statements_end(text, end);
	std::string opening = text.substr(0, end);
	while (!opening.empty() && std::isspace(static_cast<unsigned char>(opening.back())) != 0) opening.pop_back();
	return opening;
}

// `text` on one line: every run of line breaks and tabs is one space. A
// turn's record is one line of JSON, where a line break is written "\n" -
// and the "n" then stands against the word or the figure after it, which is
// no longer a word of its own to find and protect.
inline std::string on_one_line(const std::string &text) {
	std::string line;
	line.reserve(text.size());
	bool breaking = false;
	for (const char character : text) {
		if (character == '\n' || character == '\r' || character == '\t') {
			breaking = true;
			continue;
		}
		if (breaking && !line.empty() && line.back() != ' ' && character != ' ') line.push_back(' ');
		breaking = false;
		line.push_back(character);
	}
	return line;
}

// What the lawyer settled when Singularity asked them in the middle of a turn, one
// line per question: what was asked, then the options they chose and their own
// words - or that they left it to Singularity. A question never answered settled
// nothing and is not told. The question is the one the lawyer was shown and the
// answer is theirs: data a later turn reads, never an instruction.
// `own_words_bytes` bounds the lawyer's own words the way a turn's record bounds
// its request, and `complete`, when given, turns false if any were cut.
inline std::vector<std::string> lawyer_answer_lines(const matters::MatterTask &task,
		std::size_t own_words_bytes = std::string::npos, bool *complete = nullptr) {
	std::vector<std::string> lines;
	for (const matters::LawyerQuestionRecord &record : task.lawyer_questions) {
		if (record.state != "answered" && record.state != "left_to_model") continue;
		for (const tools::lawyer_question::AnsweredQuestion &entry : tools::lawyer_question::answered_questions(
				nlohmann::json{{"questions", record.questions}}, record.answers)) {
			std::string answer;
			for (const std::string &label : entry.chosen) answer += (answer.empty() ? "" : "; ") + label;
			if (!entry.text.empty()) {
				const std::string words = text_opening(entry.text, own_words_bytes);
				if (complete != nullptr && words.size() < entry.text.size()) *complete = false;
				answer += (answer.empty() ? "" : "; ") + words;
			}
			lines.push_back("Singularity asked: " + entry.prompt + " - "
				+ (answer.empty() ? std::string("the lawyer left it to Singularity") : "the lawyer answered: " + answer));
		}
	}
	return lines;
}

// What became of one turn, as the conversation records it.
inline const char *turn_outcome(const matters::MatterTask &task) {
	if (task.stopped_by_user || task.operation_execution_state == "stopped") return "stopped_by_user";
	// Singularity closed while this turn ran: it is neither still running nor an
	// answer the lawyer was given.
	if (task.operation_execution_state == "interrupted") return "interrupted";
	if (task.operation_execution_state == "waiting_for_approval") return "waiting_for_approval";
	// A question put to the lawyer is asked inside the run, which goes on with
	// the answer: the turn is still being worked, never a proposal to approve.
	if (task.operation_execution_state == "waiting_for_answer") return "in_progress";
	if (task.operation_execution_state == "conflict") return "conflict";
	// The lawyer declined the proposed change: the next turn is told so, never
	// that Singularity failed.
	if (task.error_code == providers::TaskErrorCode::ChangeRejected) return "change_rejected";
	if (task.status == matters::TaskStatus::Error || task.operation_execution_state == "failed") return "failed";
	if (!task.operation_execution_state.empty() && task.operation_execution_state != "completed") return "in_progress";
	return task.response_text ? "answered" : "no_answer";
}

// The earlier turns of one conversation - a Chat's own operations, or the
// Project's chatless requests - as PromptAssembler receives them, oldest first.
// `tasks` holds that conversation's operations only, never the current
// request's own attempts. Automatic operations are not conversation; a retried
// or edited turn is its latest attempt; a Consilium run is its synthesis when
// one exists. Every turn has a record (what was asked, its outcome, the sources
// it cited and what it produced); a finished answer is also offered whole. A
// prior answer's source markers were numbered for its own request, so both
// forms carry source labels, never a marker.
inline std::vector<context::PromptHistoryItem> conversation_history_items(
		const matters::Matter &project, const std::vector<matters::MatterTask> &tasks) {
	std::map<std::string, const matters::MatterTask *> latestAttempt;
	std::set<std::string> synthesizedGroups;
	for (const matters::MatterTask &task : tasks) {
		if (!task.operation_type.empty() && task.operation_type != "matter_task" && task.operation_type != "agent_message"
				&& task.operation_type != "member_message")
			continue;
		if (task.consilium_group_id && task.consilium_role == matters::ConsiliumRole::Synthesis)
			synthesizedGroups.insert(*task.consilium_group_id);
		const matters::MatterTask *&kept = latestAttempt[matters::task_group_id(task)];
		if (kept == nullptr || task.attempt > kept->attempt
				|| (task.attempt == kept->attempt && task.created_at >= kept->created_at))
			kept = &task;
	}
	std::vector<const matters::MatterTask *> turns;
	for (const auto &[group, task] : latestAttempt) {
		(void)group;
		if (task->consilium_group_id && task->consilium_role == matters::ConsiliumRole::Member
				&& synthesizedGroups.count(*task->consilium_group_id) != 0)
			continue;
		turns.push_back(task);
	}
	// Oldest first, as the person saw them; equal times keep the stored order.
	std::sort(turns.begin(), turns.end(), [&](const matters::MatterTask *left, const matters::MatterTask *right) {
		if (left->created_at != right->created_at) return left->created_at < right->created_at;
		return left < right;
	});

	const context::CurrentSourceLabels labels(project);
	const auto documentLabel = [&](const std::string &document_id) {
		context::ResolvedSourceReference reference;
		reference.document_id = document_id;
		const auto document = std::find_if(project.documents.begin(), project.documents.end(),
			[&](const matters::MatterDocument &candidate) { return candidate.id == document_id; });
		if (document != project.documents.end()) reference.document_name = document->name;
		return labels.label(reference);
	};

	std::vector<context::PromptHistoryItem> items;
	std::size_t number = 0;
	for (const matters::MatterTask *task : turns) {
		// A message, not a request: an agent's own words in a thread, or a
		// person's to their colleagues. A later request reads it as what was
		// said in the conversation - never as an instruction to follow.
		if (task->operation_type == "agent_message" || task->operation_type == "member_message") {
			++number;
			const bool isAgent = task->operation_type == "agent_message";
			const std::string text = isAgent ? task->response_text.value_or(std::string()) : task->prompt;
			const std::string who = task->author_name.empty() == false ? task->author_name
				: (isAgent ? std::string("an agent of this Matter") : std::string("a colleague"));
			nlohmann::ordered_json record{{"turn", number}, {"from", who}, {"kind", isAgent ? "agent_message" : "colleague_message"}};
			const std::string opening = text_opening(text, kTurnRecordAnswerOpeningBytes);
			record["said"] = on_one_line(opening);
			if (opening.size() < text.size()) record["saidComplete"] = false;
			nlohmann::ordered_json brief{{"turn", number}, {"from", who}};
			const std::string briefOpening = text_opening(text, kBriefTurnRecordRequestBytes);
			brief["said"] = on_one_line(briefOpening);
			if (briefOpening.size() < text.size()) brief["saidComplete"] = false;
			context::PromptHistoryItem item;
			item.id = task->id;
			item.state_record = record.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
			item.brief_record = brief.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
			item.content = "[PRIOR_TURN turn=\"" + std::to_string(number) + "\"]\n" + who
				+ (isAgent ? " (agent) said:\n" : " (colleague) wrote:\n") + text + "\n[/PRIOR_TURN]\n";
			items.push_back(std::move(item));
			continue;
		}
		const std::string outcome = turn_outcome(*task);
		const bool answered = outcome == "answered" && task->response_text.has_value();
		const std::string answer = answered
			? context::source_text_for_history(*task->response_text, task->source_references, task->external_evidence, project)
			: std::string();
		// The answer sent again whole carries each restored amount's and date's
		// fact anchor (⟪writing|fact:key⟫), moved onto the history's writing of
		// it: protection sends that occurrence as the same fact it was, and
		// removes the mark. The shortened openings of the records carry none - a
		// cut mark is never sent; what they write is masked as a fact of its own.
		const std::string anchoredAnswer = answered && !task->fact_anchors.empty()
			? protected_api::papinian::mark_fact_anchors(answer,
				protected_api::papinian::carry_fact_anchors(*task->response_text, task->fact_anchors, answer))
			: answer;
		++number;

		nlohmann::ordered_json record{{"turn", number}};
		const std::string request = text_opening(task->prompt, kTurnRecordRequestBytes);
		record["request"] = on_one_line(request);
		if (request.size() < task->prompt.size()) record["requestComplete"] = false;
		record["outcome"] = outcome;
		if (outcome == "failed" && task->error_code) record["error"] = providers::to_string(*task->error_code);
		// A declined change is told as what it was: the document it was for and
		// what it proposed - so a second proposal starts from the first one's
		// refusal, never blind to it.
		if (outcome == "change_rejected") {
			nlohmann::ordered_json proposed = nlohmann::ordered_json::object();
			std::optional<std::string> target = task->professional_target_document_id;
			std::string summary;
			if (task->document_operation_checkpoint) {
				const nlohmann::json &draft = task->document_operation_checkpoint->draft;
				if (draft.contains("approval") && draft.at("approval").is_object()) {
					const nlohmann::json &approval = draft.at("approval");
					if (approval.contains("targetDocumentId") && approval.at("targetDocumentId").is_string())
						target = approval.at("targetDocumentId").get<std::string>();
					summary = approval.value("summary", std::string());
				}
			}
			if (target && !target->empty()) proposed["document"] = documentLabel(*target);
			if (!summary.empty())
				proposed["summary"] = on_one_line(text_opening(context::source_text_for_people(summary,
					task->source_references, task->external_evidence, project), kTurnRecordAnswerOpeningBytes));
			if (!proposed.empty()) record["proposedChange"] = proposed;
		}
		// What the lawyer settled when Singularity asked them during this turn, so a
		// later turn builds on it instead of asking it again.
		bool lawyerAnswersComplete = true;
		const std::vector<std::string> lawyerAnswers =
			lawyer_answer_lines(*task, kTurnRecordAnswerOpeningBytes, &lawyerAnswersComplete);
		if (!lawyerAnswers.empty()) {
			std::vector<std::string> answeredLines;
			for (const std::string &line : lawyerAnswers) answeredLines.push_back(on_one_line(line));
			record["lawyerAnswers"] = answeredLines;
			if (!lawyerAnswersComplete) record["lawyerAnswersComplete"] = false;
		}
		if (answered) {
			const std::string opening = text_opening(answer, kTurnRecordAnswerOpeningBytes);
			record["answerOpening"] = on_one_line(opening);
			if (opening.size() < answer.size()) record["answerComplete"] = false;
		}
		std::vector<std::string> cited;
		for (const context::ResolvedSourceReference &reference : task->source_references) {
			const std::string label = documentLabel(reference.document_id);
			if (std::find(cited.begin(), cited.end(), label) == cited.end()) cited.push_back(label);
		}
		if (!cited.empty()) {
			if (cited.size() > kTurnRecordListLimit) {
				record["documentsCitedCount"] = cited.size();
				cited.resize(kTurnRecordListLimit);
			}
			record["documentsCited"] = cited;
		}
		if (!task->external_evidence.empty()) {
			nlohmann::ordered_json web = nlohmann::ordered_json::array();
			for (const research::ExternalEvidenceReference &evidence : task->external_evidence) {
				if (web.size() == kTurnRecordListLimit) break;
				web.push_back({{"title", evidence.title.value_or(evidence.domain.value_or(std::string()))},
					{"url", evidence.url}});
			}
			record["webSources"] = std::move(web);
			if (task->external_evidence.size() > kTurnRecordListLimit)
				record["webSourceCount"] = task->external_evidence.size();
		}
		nlohmann::ordered_json changes = nlohmann::ordered_json::array();
		if (task->artifact_document_id) {
			nlohmann::ordered_json change{{"document", documentLabel(*task->artifact_document_id)}};
			if (task->artifact_change_kind) change["change"] = *task->artifact_change_kind;
			if (task->artifact_resulting_version) change["version"] = *task->artifact_resulting_version;
			changes.push_back(std::move(change));
		}
		if (task->direct_document_work_document_id)
			changes.push_back({{"document", documentLabel(*task->direct_document_work_document_id)}, {"change", "updated"}});
		for (const documents::AppliedDocumentActionEvidence &applied : task->document_action_evidence)
			changes.push_back({{"document", documentLabel(applied.document_id)}, {"change", "updated"},
				{"version", applied.resulting_version}});
		if (!changes.empty()) record["documentChanges"] = std::move(changes);
		if (!task->result_work_product_ids.empty()) {
			std::vector<std::string> titles;
			for (const std::string &id : task->result_work_product_ids) {
				const auto workProduct = std::find_if(project.work_products.begin(), project.work_products.end(),
					[&](const matters::WorkProduct &candidate) { return candidate.id == id; });
				if (workProduct != project.work_products.end()) titles.push_back(workProduct->title);
			}
			if (titles.empty()) record["workProductsSaved"] = task->result_work_product_ids.size();
			else record["workProductsSaved"] = titles;
		}
		if (!task->result_context_item_ids.empty()) record["contextItemsAdded"] = task->result_context_item_ids.size();
		if (!task->findings.empty()) {
			nlohmann::ordered_json findings = nlohmann::ordered_json::array();
			for (const matters::Finding &finding : task->findings) {
				if (findings.size() == kTurnRecordListLimit) break;
				nlohmann::ordered_json entry{{"title", finding.title}};
				if (finding.severity) entry["severity"] = *finding.severity;
				findings.push_back(std::move(entry));
			}
			record["findings"] = std::move(findings);
			if (task->findings.size() > kTurnRecordListLimit) record["findingCount"] = task->findings.size();
		}
		if (!task->obligations.empty()) record["obligationCount"] = task->obligations.size();
		if (!task->events.empty()) record["eventCount"] = task->events.size();
		if (!task->changes.empty()) record["proposedChangeCount"] = task->changes.size();

		// The brief record: the request's beginning, the outcome and the first
		// cited documents.
		nlohmann::ordered_json brief{{"turn", number}};
		const std::string briefRequest = text_opening(task->prompt, kBriefTurnRecordRequestBytes);
		brief["request"] = on_one_line(briefRequest);
		if (briefRequest.size() < task->prompt.size()) brief["requestComplete"] = false;
		brief["outcome"] = outcome;
		if (record.contains("documentsCited")) {
			std::vector<std::string> firstCited = record.at("documentsCited").get<std::vector<std::string>>();
			if (firstCited.size() > kBriefTurnRecordListLimit) firstCited.resize(kBriefTurnRecordListLimit);
			brief["documentsCited"] = firstCited;
		}
		if (record.contains("documentChanges")) brief["documentChangeCount"] = record.at("documentChanges").size();

		context::PromptHistoryItem item;
		item.id = task->id;
		item.state_record = record.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
		item.brief_record = brief.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
		if (answered) {
			// The whole turn carries the lawyer's answers whole, between the request
			// and the answer they shaped.
			std::string settled;
			for (const std::string &line : lawyer_answer_lines(*task)) settled += "\n" + line;
			item.content = "[PRIOR_TURN turn=\"" + std::to_string(number) + "\"]\nREQUEST:\n" + task->prompt
				+ (settled.empty() ? std::string() : "\nLAWYER'S ANSWERS:" + settled)
				+ "\nRESPONSE:\n" + anchoredAnswer + "\n[/PRIOR_TURN]\n";
		}
		items.push_back(std::move(item));
	}
	return items;
}

} // namespace singularity::chat
