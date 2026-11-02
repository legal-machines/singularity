#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Context/ControlText/ControlText.hpp"
#include "Platform/TextFold/TextFold.hpp"
#include "Tool/ToolBroker/ToolBroker.hpp"

#include <nlohmann/json.hpp>

// A question only the lawyer can answer, put to them in the middle of a run
// and answered in that same run (tool lawyer.ask): which of two documents,
// whose position, which of two readings of the request.
//
// The model never calls a tool for it. The question rides on its answer as the
// nullable lawyerQuestion of professional_response, exactly like
// projectEvidenceRequest, so it is restored for display in the same pass as the
// answer, and the lawyer's reply returns to the model inside the same protected
// request text as everything else of the operation. ToolBroker owns the pause:
// the question waits as a pending approval that always asks, whatever the
// Project's action mode, and Stop or Singularity closing wakes it the way they wake
// any approval.
//
// Singularity, not the model, decides the shape the lawyer sees: one to three
// questions of two to four options each, the ids they are answered by, and that
// the lawyer may always answer in their own words.
namespace singularity::tools::lawyer_question {

inline constexpr const char *kToolId = "lawyer.ask";
inline constexpr std::size_t kMaximumQuestions = 3;
inline constexpr std::size_t kMinimumOptions = 2;
inline constexpr std::size_t kMaximumOptions = 4;
// Counted in characters (Unicode code points), never bytes: a question in
// Russian or Japanese gets the room one in English gets.
inline constexpr std::size_t kMaximumTitleCharacters = 120;
inline constexpr std::size_t kMaximumPromptCharacters = 300;
inline constexpr std::size_t kMaximumLabelCharacters = 80;
inline constexpr std::size_t kMaximumNoteCharacters = 240;
inline constexpr std::size_t kMaximumAnswerCharacters = 2000;

// The lawyerQuestion field of professional_response. No enum or const text and
// no ids of the model's own: a literal in the schema could never be restored
// after protection, and Singularity numbers the questions itself. The counts are
// Singularity's to check (normalize() below), never the schema's.
inline nlohmann::json schema() {
	const nlohmann::json nullableText{{"type", nlohmann::json::array({"string", "null"})}};
	const nlohmann::json option{{"type", "object"}, {"additionalProperties", false},
		{"properties", {{"label", {{"type", "string"}}}, {"note", nullableText}}},
		{"required", nlohmann::json::array({"label", "note"})}};
	const nlohmann::json question{{"type", "object"}, {"additionalProperties", false},
		{"properties", {{"prompt", {{"type", "string"}}}, {"allowsSeveral", {{"type", "boolean"}}},
			{"options", {{"type", "array"}, {"items", option}}}}},
		{"required", nlohmann::json::array({"prompt", "allowsSeveral", "options"})}};
	return nlohmann::json{{"type", nlohmann::json::array({"object", "null"})}, {"additionalProperties", false},
		{"properties", {{"title", nullableText}, {"questions", {{"type", "array"}, {"items", question}}}}},
		{"required", nlohmann::json::array({"title", "questions"})}};
}

// What the model is told it can do: required instruction text beside the rest
// of the request's CAPABILITY section, and the first offer Singularity withdraws
// when the request cannot hold it.
inline const std::string &protocol() {
	static const std::string text = "Only when the task cannot be done without a choice only the lawyer can make - "
		"which of two documents to change, whose position to take, a figure the documents leave undecided that the "
		"document must state - set lawyerQuestion instead of asking in prose or "
		"guessing: 1-3 short questions, each with 2-4 short options, the likeliest first, never \"Other\" (the "
		"lawyer can always answer in their own words). Singularity pauses and gives you the answer here; while asking, "
		"response is one sentence, actionType \"none\". A question about the documents is answered, never asked "
		"back. Never ask what the request, the Project or the conversation settles: \"it\" or \"the document\" "
		"means what the latest turns were about; a declined change asked for again is proposed again. Otherwise "
		"lawyerQuestion is null.";
	return text;
}

namespace detail {

inline std::string trimmed(const std::string &text) {
	const std::size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return std::string();
	const std::size_t last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

inline std::size_t characters(const std::string &text) {
	std::size_t count = 0;
	for (const char byte : text)
		if ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U) ++count;
	return count;
}

// A source handle means nothing to the person asked: it is removed, with the
// space before it, rather than the question refused for carrying one.
inline std::string without_source_markers(const std::string &text) {
	std::string result;
	std::size_t at = 0;
	for (;;) {
		const std::size_t marker = text.find("[SOURCE:", at);
		const std::size_t end = marker == std::string::npos ? std::string::npos : text.find(']', marker);
		if (end == std::string::npos) {
			result.append(text, at, std::string::npos);
			return result;
		}
		result.append(text, at, marker - at);
		while (!result.empty() && (result.back() == ' ' || result.back() == '\t')) result.pop_back();
		at = end + 1;
	}
}

inline std::string cleaned(const std::string &text) {
	return trimmed(without_source_markers(text));
}

// A text the question may carry, cleaned; nullopt when the value is no text.
inline std::optional<std::string> text_of(const nlohmann::json &value, const char *key) {
	if (!value.contains(key) || !value.at(key).is_string()) return std::nullopt;
	return cleaned(value.at(key).get<std::string>());
}

// An option that only says "something else" - Other, Other (specify), Другое -
// repeats the field every question already has for the lawyer's own words, so
// it is left out rather than shown twice.
inline bool only_says_other(const std::string &label) {
	std::string words;
	for (const char byte : platform::folded_text(label)) {
		const unsigned char value = static_cast<unsigned char>(byte);
		if (value < 0x80U && std::isalpha(value) == 0) {
			if (!words.empty() && words.back() != ' ') words += ' ';
			continue;
		}
		words += byte;
	}
	while (!words.empty() && words.back() == ' ') words.pop_back();
	static const std::set<std::string> other{"other", "other specify", "other please specify", "something else",
		"none of these", "none of the above", "\u0434\u0440\u0443\u0433\u043e\u0435",
		"\u0434\u0440\u0443\u0433\u043e\u0435 \u0443\u043a\u0430\u0436\u0438\u0442\u0435",
		"\u0434\u0440\u0443\u0433\u043e\u0439 \u0432\u0430\u0440\u0438\u0430\u043d\u0442"};
	return other.count(words) > 0;
}

} // namespace detail

// Whether a lawyerQuestion value asks anything at all: null, anything that is
// not an object, or an empty list of questions is the model saying it has no
// question.
inline bool asks_anything(const nlohmann::json &value) {
	return value.is_object() && value.contains("questions") && value.at("questions").is_array()
		&& !value.at("questions").empty();
}

// The question as the lawyer is shown it - {title, questions: [{id, prompt,
// note, choice, allowsOwnAnswer, options: [{id, label, note}]}]} - or, when it
// cannot be put to them, the reason: a category the model is told when it is
// given its one chance to ask again.
struct NormalizedQuestion {
	std::optional<nlohmann::json> question;
	std::string issue;
};

inline NormalizedQuestion normalize(const nlohmann::json &value) {
	NormalizedQuestion result;
	if (!asks_anything(value)) {
		result.issue = "no_question";
		return result;
	}
	const nlohmann::json &questions = value.at("questions");
	if (questions.size() > kMaximumQuestions) {
		result.issue = "too_many_questions";
		return result;
	}
	nlohmann::json normalized{{"title", nullptr}, {"questions", nlohmann::json::array()}};
	const std::optional<std::string> title = detail::text_of(value, "title");
	if (title && !title->empty()) {
		if (detail::characters(*title) > kMaximumTitleCharacters) {
			result.issue = "title_too_long";
			return result;
		}
		normalized["title"] = *title;
	}
	for (std::size_t index = 0; index < questions.size(); ++index) {
		const nlohmann::json &asked = questions.at(index);
		if (!asked.is_object()) {
			result.issue = "question_not_an_object";
			return result;
		}
		const std::optional<std::string> prompt = detail::text_of(asked, "prompt");
		if (!prompt || prompt->empty()) {
			result.issue = "question_without_prompt";
			return result;
		}
		if (detail::characters(*prompt) > kMaximumPromptCharacters) {
			result.issue = "question_too_long";
			return result;
		}
		if (!asked.contains("options") || !asked.at("options").is_array()
				|| asked.at("options").size() < kMinimumOptions) {
			result.issue = "too_few_options";
			return result;
		}
		if (asked.at("options").size() > kMaximumOptions) {
			result.issue = "too_many_options";
			return result;
		}
		const std::string questionId = "q" + std::to_string(index + 1);
		const bool allowsSeveral = asked.contains("allowsSeveral") && asked.at("allowsSeveral").is_boolean()
			&& asked.at("allowsSeveral").get<bool>();
		nlohmann::json options = nlohmann::json::array();
		std::set<std::string> labels;
		for (std::size_t optionIndex = 0; optionIndex < asked.at("options").size(); ++optionIndex) {
			const nlohmann::json &offered = asked.at("options").at(optionIndex);
			const std::optional<std::string> label = offered.is_object()
				? detail::text_of(offered, "label") : std::optional<std::string>();
			if (!label || label->empty()) {
				result.issue = "option_without_label";
				return result;
			}
			if (detail::characters(*label) > kMaximumLabelCharacters) {
				result.issue = "option_too_long";
				return result;
			}
			if (!labels.insert(*label).second) {
				result.issue = "duplicate_option";
				return result;
			}
			const std::optional<std::string> note = detail::text_of(offered, "note");
			if (note && detail::characters(*note) > kMaximumNoteCharacters) {
				result.issue = "note_too_long";
				return result;
			}
			if (detail::only_says_other(*label)) continue;
			options.push_back({{"id", questionId + "o" + std::to_string(options.size() + 1)}, {"label", *label},
				{"note", note && !note->empty() ? nlohmann::json(*note) : nlohmann::json(nullptr)}});
		}
		if (options.empty()) {
			result.issue = "too_few_options";
			return result;
		}
		// The lawyer can always answer in their own words: that is Singularity's
		// rule, never the model's choice.
		normalized["questions"].push_back({{"id", questionId}, {"prompt", *prompt}, {"note", nullptr},
			{"choice", allowsSeveral ? "several" : "one"}, {"allowsOwnAnswer", true}, {"options", std::move(options)}});
	}
	result.question = std::move(normalized);
	return result;
}

// Why an answer cannot be taken for this (normalized) question, or nothing when
// it can. A question is answered by one of its options, by several where it
// takes several, or in the lawyer's own words - or skipped, which leaves it to
// Singularity. Null leaves them all to Singularity and can always be taken.
inline std::optional<std::string> answers_issue(const nlohmann::json &question, const nlohmann::json &answers) {
	if (answers.is_null()) return std::nullopt;
	if (!answers.is_object()) return std::string("answer_not_readable");
	if (!question.is_object() || !question.contains("questions") || !question.at("questions").is_array())
		return std::string("unknown_question");
	const nlohmann::json &questions = question.at("questions");
	for (auto entry = answers.begin(); entry != answers.end(); ++entry) {
		const auto asked = std::find_if(questions.begin(), questions.end(), [&](const nlohmann::json &candidate) {
			return candidate.value("id", std::string()) == entry.key();
		});
		if (asked == questions.end()) return std::string("unknown_question");
		const nlohmann::json &answer = entry.value();
		if (!answer.is_object()) return std::string("answer_not_readable");
		if (answer.contains("text") && !answer.at("text").is_string()) return std::string("answer_not_readable");
		if (answer.contains("optionIds") && !answer.at("optionIds").is_array()) return std::string("answer_not_readable");
		std::set<std::string> chosen;
		if (answer.contains("optionIds")) {
			for (const nlohmann::json &optionId : answer.at("optionIds")) {
				if (!optionId.is_string()) return std::string("answer_not_readable");
				const std::string id = optionId.get<std::string>();
				const nlohmann::json &options = asked->value("options", nlohmann::json::array());
				const bool known = std::any_of(options.begin(), options.end(), [&](const nlohmann::json &option) {
					return option.value("id", std::string()) == id;
				});
				if (!known) return std::string("unknown_option");
				if (!chosen.insert(id).second) return std::string("duplicate_choice");
			}
		}
		if (chosen.size() > 1 && asked->value("choice", std::string("one")) != "several")
			return std::string("one_choice_only");
		const std::string text = answer.contains("text") ? answer.at("text").get<std::string>() : std::string();
		if (detail::characters(text) > kMaximumAnswerCharacters) return std::string("answer_too_long");
	}
	// A question the lawyer skipped is left to Singularity; an answer that settles
	// none of them is "Decide these yourself" sent the long way, and is taken
	// as that - null - rather than recorded as an answer that says nothing.
	const bool answersAny = std::any_of(questions.begin(), questions.end(), [&](const nlohmann::json &asked) {
		const std::string id = asked.value("id", std::string());
		if (!answers.contains(id)) return false;
		const nlohmann::json &answer = answers.at(id);
		const bool chose = answer.contains("optionIds") && !answer.at("optionIds").empty();
		const bool wrote = answer.contains("text") && !detail::trimmed(answer.at("text").get<std::string>()).empty();
		return chose || wrote;
	});
	if (!answersAny) return std::string("unanswered_question");
	return std::nullopt;
}

// What the lawyer is told when Singularity cannot take their answer. The question
// stays open, so they can answer again.
inline std::string answer_issue_message(const std::string &issue) {
	if (issue == "unanswered_question")
		return "Answer at least one question, or leave them to Singularity.";
	if (issue == "one_choice_only") return "Choose one answer for a question that takes one.";
	if (issue == "answer_too_long")
		return "Keep each answer under " + std::to_string(kMaximumAnswerCharacters) + " characters.";
	return "This answer does not match the question Singularity asked. Answer the question shown above.";
}

// The answer as it is kept and read back: for each question its chosen option
// ids, in the order the question offers them, and the lawyer's own words.
inline nlohmann::json normalized_answers(const nlohmann::json &question, const nlohmann::json &answers) {
	if (!answers.is_object() || !question.contains("questions")) return nullptr;
	nlohmann::json result = nlohmann::json::object();
	for (const nlohmann::json &asked : question.at("questions")) {
		const std::string id = asked.value("id", std::string());
		if (!answers.contains(id) || !answers.at(id).is_object()) continue;
		const nlohmann::json &answer = answers.at(id);
		nlohmann::json optionIds = nlohmann::json::array();
		const nlohmann::json chosen = answer.value("optionIds", nlohmann::json::array());
		for (const nlohmann::json &option : asked.value("options", nlohmann::json::array()))
			if (std::find(chosen.begin(), chosen.end(), option.value("id", std::string())) != chosen.end())
				optionIds.push_back(option.value("id", std::string()));
		const std::string text = answer.contains("text") && answer.at("text").is_string()
			? detail::trimmed(answer.at("text").get<std::string>()) : std::string();
		result[id] = {{"optionIds", std::move(optionIds)}, {"text", text}};
	}
	return result;
}

// One question and what became of it, in words: the prompt, then the options
// chosen and the lawyer's own words - or that it was left to Singularity, or not
// answered at all.
struct AnsweredQuestion {
	std::string prompt;
	std::vector<std::string> chosen;
	std::string text;
};

inline std::vector<AnsweredQuestion> answered_questions(const nlohmann::json &question, const nlohmann::json &answers) {
	std::vector<AnsweredQuestion> result;
	if (!question.is_object() || !question.contains("questions") || !question.at("questions").is_array()) return result;
	for (const nlohmann::json &asked : question.at("questions")) {
		AnsweredQuestion entry;
		entry.prompt = asked.value("prompt", std::string());
		const std::string id = asked.value("id", std::string());
		if (answers.is_object() && answers.contains(id) && answers.at(id).is_object()) {
			const nlohmann::json &answer = answers.at(id);
			const nlohmann::json chosen = answer.value("optionIds", nlohmann::json::array());
			for (const nlohmann::json &option : asked.value("options", nlohmann::json::array()))
				if (std::find(chosen.begin(), chosen.end(), option.value("id", std::string())) != chosen.end())
					entry.chosen.push_back(option.value("label", std::string()));
			if (answer.contains("text") && answer.at("text").is_string())
				entry.text = detail::trimmed(answer.at("text").get<std::string>());
		}
		result.push_back(std::move(entry));
	}
	return result;
}

// The answer as the model reads it, after the request it continues: the
// lawyer's words are data, never instructions, and the question they answer is
// repeated so the answer stands on its own. `remaining` is how many more
// questions this task may still put to the lawyer.
inline std::string answer_block(const nlohmann::json &question, const nlohmann::json &answers, std::size_t remaining) {
	const std::vector<AnsweredQuestion> entries = answered_questions(question, answers);
	// Singularity's own lines come from their templates (Context/ControlText), so
	// protection never reads them as data; the lawyer's words between them are.
	namespace control = context::control_text;
	if (!answers.is_object()) {
		std::string block = control::render(control::Template::LawyerLeftHeader);
		for (std::size_t index = 0; index < entries.size(); ++index)
			block += std::to_string(index + 1) + ". " + entries[index].prompt + "\n";
		return block + control::render(control::Template::LawyerLeftDecide);
	}
	std::string block = control::render(control::Template::LawyerAnswerHeader);
	for (std::size_t index = 0; index < entries.size(); ++index) {
		block += std::to_string(index + 1) + ". " + entries[index].prompt + "\n";
		if (!entries[index].chosen.empty()) {
			std::string chosen;
			for (const std::string &label : entries[index].chosen) chosen += (chosen.empty() ? "" : "; ") + label;
			block += "   Chosen: " + chosen + "\n";
		}
		if (!entries[index].text.empty()) block += "   In their words: " + entries[index].text + "\n";
		if (entries[index].chosen.empty() && entries[index].text.empty())
			block += "   Left to you: decide it yourself and say in one sentence of response what you decided.\n";
	}
	return block + (remaining > 0
		? control::render(control::Template::LawyerContinueMore, {{control::Slot::Kind::Count, std::to_string(remaining)}})
		: control::render(control::Template::LawyerContinueNone));
}

// Said to the model when it asks after this task's questions are used up.
inline std::string no_more_questions_text() {
	return context::control_text::render(context::control_text::Template::LawyerNoMoreQuestions);
}

// Said to the model when Singularity cannot put its question as it was written:
// once it may ask again, after that it decides for itself.
inline std::string question_refused_text(const std::string &issue, bool mayAskAgain) {
	if (mayAskAgain)
		return "\nSINGULARITY: That question could not be put to the lawyer (" + issue + "). Ask again with 1-"
			+ std::to_string(kMaximumQuestions) + " plain questions of " + std::to_string(kMinimumOptions) + "-"
			+ std::to_string(kMaximumOptions) + " short options each, or decide yourself and complete the request.\n";
	return "\nSINGULARITY: That question could not be put to the lawyer either (" + issue + "). Decide it yourself, say in "
		"one sentence of response what you assumed, set lawyerQuestion to null and complete the request.\n";
}

// lawyer.ask as ToolBroker runs it. It proposes nothing and sends nothing
// anywhere (a LocalProposal), but it always asks: a question is put to the
// lawyer in every action mode, Full Project Access included, because only the
// lawyer can answer it. Its approval carries the question as the lawyer sees
// it, and the lawyer's answer comes back as the approval's reply - the only
// reply any tool is handed.
inline ToolDefinition tool_definition() {
	ToolDefinition definition;
	definition.id = kToolId;
	definition.capability = kToolId;
	definition.effect = ToolEffectClass::LocalProposal;
	definition.approval_summary = "Singularity needs your answer before it goes on.";
	definition.validate = [](const ToolExecutionContext &, const nlohmann::json &arguments) -> std::optional<std::string> {
		const NormalizedQuestion question = normalize(arguments);
		if (!question.question) return question.issue;
		if (arguments.is_object() && arguments.contains("answers"))
			return answers_issue(*question.question, arguments.at("answers"));
		return std::nullopt;
	};
	definition.always_asks = [](const ToolExecutionContext &, const nlohmann::json &) { return true; };
	definition.takes_response = true;
	definition.describe_approval_target = [](const ToolExecutionContext &, const nlohmann::json &arguments,
			ToolApprovalRequest &approval) {
		const NormalizedQuestion question = normalize(arguments);
		if (question.question) approval.question_json = question.question->dump();
	};
	definition.execute = [](const ToolExecutionContext &, const nlohmann::json &arguments) {
		const NormalizedQuestion question = normalize(arguments);
		const nlohmann::json answers = arguments.is_object() && arguments.contains("answers")
			? arguments.at("answers") : nlohmann::json();
		return nlohmann::json{{"answers", question.question ? normalized_answers(*question.question, answers)
			: nlohmann::json()}};
	};
	return definition;
}

// Registers lawyer.ask with the production ToolBroker. Registering again only
// replaces the definition with the same one, so an operation that offers a
// question registers it every time and never finds it missing after a test
// cleared the broker.
inline void ensure_production_lawyer_question_tool() {
	production_tool_broker().register_tool(tool_definition());
}

} // namespace singularity::tools::lawyer_question
