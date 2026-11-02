#include "Context/PromptAssembler/PromptAssembler.hpp"

#include "Routing/LegalTaskPresets/LegalTaskPresets.hpp"
#include "Routing/ProtectedInference/SourceMaterialSpans.hpp"
#include "Routing/SystemPromptProfiles/SystemPromptProfiles.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace singularity::context {
namespace {

bool blank(std::string_view value) {
  return value.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

void append_section(std::string &prompt, std::string_view name,
                    std::string_view content) {
  prompt += "<<<LM_SECTION:";
  prompt += name;
  prompt += ">>>\n";
  // Reserve LM boundary tokens for the assembler. Source documents and user
  // instructions can quote arbitrary text, including a forged closing marker;
  // render such occurrences visibly as literals instead of structural tokens.
  std::string escaped(content);
  std::size_t marker = 0;
  while ((marker = escaped.find("<<<LM_", marker)) != std::string::npos) {
    escaped.replace(marker, 6, "<<<LM_LITERAL_");
    marker += 14;
  }
  prompt += escaped;
  prompt += "\n<<<LM_END_SECTION:";
  prompt += name;
  prompt += ">>>\n\n";
}

// The only TASK text a plain-chat turn ever sees - never the General
// preset's "complete the legal task requested by the user", which has
// nothing to point at for an ordinary message and has driven a real local
// model to fabricate a legal-task clarification request for a bare
// greeting. See PromptAssemblyInput::plain_chat.
constexpr std::string_view kPlainChatTaskText =
    "No specific legal task is selected for this message, and none is "
    "implied by it. Reply naturally and helpfully to what the user actually "
    "wrote, the way any capable assistant would. Only produce legal "
    "analysis, drafting or a structured work product if the user's own "
    "message actually asks for that.";
constexpr std::string_view kPlainChatOutputStyle =
    "A natural, direct reply - no forced legal framing, no request for a "
    "corporate question or risk to analyze.";

std::string task_text(const LegalTaskPreset &preset, bool plain_chat, bool document_bound) {
  if (plain_chat) return std::string(kPlainChatTaskText);
  std::string task;
  task += "Preset: ";
  task += preset.name;
  task += "\nPreset ID: ";
  task += preset.id;
  task += "\nPreset version: ";
  task += std::to_string(preset.version);
  task += "\nInstructions: ";
  task += preset.instructions;
  task += "\nExpected output style: ";
  task += preset.expected_output_style;
  // A preset's expected output style is Singularity's own generic description of
  // a deliverable's shape - written without any knowledge of the document
  // actually being worked on. When the work IS bound to a real document that
  // already establishes its own convention, the document wins: a firm's real
  // risk register is the authority on how that firm records a risk, not this
  // sentence. Without this, "a risk register ordered by materiality with
  // columns for severity (High, Medium or Low)" silently competes with a
  // document whose own real register is structured differently and grades
  // severity in its own language - a conflict a lawyer then has to undo by
  // hand. The system profile (legal-machines/4) states the same precedence
  // generally; this states it exactly where the competing instruction is.
  if (document_bound)
    task += "\nThis output style is generic: where the bound document already establishes its own structure, "
            "terminology or severity wording, follow the document instead and keep only the substance above. "
            "An explicit user instruction overrides both.";
  return task;
}

constexpr std::string_view excluded_text = kExcludedByBudgetPlaceholder;

struct RenderState {
  bool task = false;
  // The bound candidate's preview: a bounded reading of what a change could
  // target, given when the window holds it and left out when it does not (the
  // full content is authoritative at commit time either way).
  bool candidate_context = false;
  bool user_instructions = false;
  bool matter_instructions = false;
  bool legacy_history = false;
  std::vector<bool> sources;
  std::vector<bool> history_items;
  std::vector<bool> history_records;
  std::vector<bool> history_briefs;
	std::vector<bool> project_context_items;
	std::size_t excluded_selected_sources = 0;
};

bool offered_context_item(const PromptContextItem &item) {
  return !item.id.empty() && !blank(item.content);
}

bool offered_turn(const PromptHistoryItem &item) {
  return !item.id.empty() && (!blank(item.content) || !blank(item.state_record) || !blank(item.brief_record));
}

// The record a turn not delivered whole is rendered with, if any.
const std::string *rendered_turn_record(const PromptHistoryItem &item, const RenderState &state, std::size_t index) {
  if (state.history_items[index]) return nullptr;
  if (state.history_records[index] && !blank(item.state_record)) return &item.state_record;
  if (state.history_briefs[index] && !blank(item.brief_record)) return &item.brief_record;
  return nullptr;
}

// "turn 3", "turns 1-4, 7".
std::string turn_list(const std::vector<std::size_t> &numbers) {
  std::string text = numbers.size() == 1 ? "turn " : "turns ";
  for (std::size_t index = 0; index < numbers.size();) {
    std::size_t end = index;
    while (end + 1 < numbers.size() && numbers[end + 1] == numbers[end] + 1) ++end;
    if (index > 0) text += ", ";
    text += std::to_string(numbers[index]);
    if (end > index) text += "-" + std::to_string(numbers[end]);
    index = end + 1;
  }
  return text;
}

// Backend-authored statement of what this request carries of Project Context
// and of the conversation's earlier turns (numbered oldest first). Numbers and
// fixed wording only - never Project data.
std::string request_coverage_text(const PromptAssemblyInput &input, const RenderState &state) {
  std::string text;
  std::size_t contextTotal = 0;
  std::size_t contextIncluded = 0;
  for (std::size_t index = 0; index < input.project_context_items.size(); ++index) {
    if (!offered_context_item(input.project_context_items[index])) continue;
    ++contextTotal;
    if (state.project_context_items[index]) ++contextIncluded;
  }
  if (contextIncluded < contextTotal) {
    text += contextIncluded == 0
        ? "Project Context: none of its " + std::to_string(contextTotal)
            + " items fit this model's context window, so none is included."
        : "Project Context: " + std::to_string(contextIncluded) + " of " + std::to_string(contextTotal)
            + " items are included; the other " + std::to_string(contextTotal - contextIncluded)
            + " did not fit this model's context window.";
  }
  std::vector<std::size_t> full;
  std::vector<std::size_t> records;
  std::vector<std::size_t> omitted;
  std::size_t turns = 0;
  for (std::size_t index = 0; index < input.history_items.size(); ++index) {
    if (!offered_turn(input.history_items[index])) continue;
    const std::size_t number = ++turns;
    if (state.history_items[index]) full.push_back(number);
    else if (rendered_turn_record(input.history_items[index], state, index) != nullptr) records.push_back(number);
    else omitted.push_back(number);
  }
  if (turns == 0) return text;
  if (!text.empty()) text += "\n";
  if (full.size() == turns) {
    return text + (turns == 1 ? std::string("Conversation: its 1 earlier turn is included in full in HISTORY.")
        : "Conversation: all " + std::to_string(turns) + " earlier turns are included in full in HISTORY.");
  }
  text += "Conversation: " + std::to_string(turns) + (turns == 1 ? " earlier turn." : " earlier turns.");
  if (!full.empty()) text += " Included in full in HISTORY: " + turn_list(full) + ".";
  if (!records.empty())
    text += " Included only as records in CONVERSATION_STATE, without their full answers: " + turn_list(records) + ".";
  if (!omitted.empty())
    text += " Not included, because they did not fit this model's context window: " + turn_list(omitted) + ".";
  return text;
}

// Backend-authored coverage statement, outside the untrusted SOURCE_MATERIAL
// section: whether each selected document (by its provider-facing handle
// only) was supplied completely, partially or not at all. The model must never
// read a context-limited selection as the complete selected material.
// `worst_case` renders every document as partial at full digit width - the
// largest this statement can become for the selection.
std::string source_coverage_text(const std::vector<PromptSourceMaterial> &sources, const RenderState &state,
		RequiredSourceOrigin origin, bool worst_case = false) {
	const bool selection = origin == RequiredSourceOrigin::UserSelection;
	struct DocumentCoverage {
		std::string label;
		std::size_t supplied = 0;
		std::size_t total = 0;
	};
	std::vector<DocumentCoverage> documents;
	std::unordered_map<std::string, std::size_t> position_of_document;
	for (std::size_t index = 0; index < sources.size(); ++index) {
		if (sources[index].explicitly_selected == false) continue;
		const std::string key = sources[index].document_id.empty()
			? "#" + std::to_string(index) : sources[index].document_id;
		auto position = position_of_document.find(key);
		if (position == position_of_document.end()) {
			position = position_of_document.emplace(key, documents.size()).first;
			DocumentCoverage coverage;
			coverage.label = sources[index].document_label.empty()
				? (selection ? "Selected document " : "Document ") + std::to_string(documents.size() + 1)
				: sources[index].document_label;
			documents.push_back(std::move(coverage));
		}
		DocumentCoverage &coverage = documents[position->second];
		++coverage.total;
		if (state.sources[index] == true) ++coverage.supplied;
	}
	std::string text = selection
		? "SOURCE_MATERIAL does not hold every passage of the user's selected documents: the context available for "
		  "this request could not fit them all. Coverage by selected document:"
		: "SOURCE_MATERIAL does not hold every passage of the documents this request must re-read: the context "
		  "available for this request could not fit them all. Coverage by document:";
	for (const DocumentCoverage &coverage : documents) {
		const std::string total = std::to_string(coverage.total);
		text += "\n- " + coverage.label + ": ";
		if (worst_case == false && coverage.supplied == coverage.total)
			text += "complete (all " + total + " passages supplied)";
		else if (worst_case == false && coverage.supplied == 0)
			text += "not supplied (0 of " + total + " passages)";
		else
			text += "partial (" + (worst_case ? total : std::to_string(coverage.supplied)) + " of " + total
				+ " passages supplied)";
	}
	text += std::string("\nAn omitted passage is missing only from this request, not from its document: never state that a ")
		+ (selection ? "selected document" : "document") + " lacks something that could be in its omitted passages. "
		"Where the omission matters, "
		"declare it as an evidence gap in plain words; handles are internal: never write one in any form, such as "
		"\"Document 1\".";
	return text;
}

// Backend-authored statement for passages Singularity chose by relevance rather
// than a person selecting them: what was read, and - the part a model must
// never assume - what was not. Document handles only, never names.
std::string relevance_coverage_text(const PromptAssemblyInput &input,
		const std::vector<PromptSourceMaterial> &sources, const RenderState &state) {
	struct DocumentCoverage {
		std::string label;
		std::size_t supplied = 0;
		std::size_t retrieved = 0;
	};
	std::vector<DocumentCoverage> documents;
	std::unordered_map<std::string, std::size_t> position_of_document;
	std::size_t supplied = 0;
	for (std::size_t index = 0; index < sources.size(); ++index) {
		if (sources[index].explicitly_selected) continue;
		const std::string key = sources[index].document_id.empty()
			? "#" + std::to_string(index) : sources[index].document_id;
		auto position = position_of_document.find(key);
		if (position == position_of_document.end()) {
			position = position_of_document.emplace(key, documents.size()).first;
			DocumentCoverage coverage;
			coverage.label = sources[index].document_label.empty()
				? "Document " + std::to_string(documents.size() + 1) : sources[index].document_label;
			documents.push_back(std::move(coverage));
		}
		DocumentCoverage &coverage = documents[position->second];
		++coverage.retrieved;
		if (state.sources[index]) {
			++coverage.supplied;
			++supplied;
		}
	}
	if (documents.empty()) return std::string();
	std::string text = "SOURCE_MATERIAL holds the passages Singularity judged most relevant to this request - "
		+ std::to_string(supplied) + " passage" + (supplied == 1 ? "" : "s") + " from "
		+ std::to_string(documents.size()) + " document" + (documents.size() == 1 ? "" : "s");
	if (input.relevance_candidate_documents > documents.size())
		text += " of " + std::to_string(input.relevance_candidate_documents) + " readable in this Project";
	text += " - never the whole Project. Coverage by document:";
	for (const DocumentCoverage &coverage : documents) {
		text += "\n- " + coverage.label + ": " + std::to_string(coverage.supplied) + " of "
			+ std::to_string(coverage.retrieved) + " retrieved passage"
			+ (coverage.retrieved == 1 ? "" : "s") + " supplied";
	}
	text += "\nPassages and documents not listed here were not read for this request. Never state that the Project "
		"or a document lacks something that could be in them: say what was checked, and declare the rest as an "
		"evidence gap where it matters. Handles are internal: never write one in any form, such as \"Document 1\".";
	return text;
}

// The exact byte length append_section() gives `content` once its reserved
// boundary tokens are escaped.
std::size_t escaped_section_bytes(std::string_view content) {
	std::size_t bytes = content.size();
	std::size_t marker = 0;
	while ((marker = content.find("<<<LM_", marker)) != std::string_view::npos) {
		bytes += 8;
		marker += 6;
	}
	return bytes;
}

std::string render_prompt(const PromptAssemblyInput &input,
                          const SystemPromptProfile &profile,
                          const LegalTaskPreset &preset,
                          const std::vector<PromptSourceMaterial> &sources,
                          const RenderState &state) {
  std::string prompt;
  prompt.reserve(profile.instructions.size() + input.current_request.size() +
                 1024);
  append_section(prompt, "SYSTEM", profile.instructions);
  append_section(prompt, "USER_DEFAULT_INSTRUCTIONS",
                 input.user_default_instructions &&
                         !blank(*input.user_default_instructions)
                     ? (state.user_instructions
                            ? std::string_view(*input.user_default_instructions)
                            : excluded_text)
                     : std::string_view("(none supplied)"));
  append_section(prompt, "MATTER_INSTRUCTIONS",
                 input.matter_instructions && !blank(*input.matter_instructions)
                     ? (state.matter_instructions
                            ? std::string_view(*input.matter_instructions)
                            : excluded_text)
                     : std::string_view("(none supplied)"));
  const auto task = task_text(preset, input.plain_chat,
      input.candidate_context.has_value() && !blank(*input.candidate_context));
  append_section(prompt, "TASK", state.task ? std::string_view(task)
                                             : excluded_text);
	if (input.capability_instructions && !blank(*input.capability_instructions))
		append_section(prompt, "CAPABILITY", *input.capability_instructions);
	if (input.candidate_context && !blank(*input.candidate_context))
		append_section(prompt, "CANDIDATE_CONTEXT", state.candidate_context ? std::string_view(*input.candidate_context) : excluded_text);

	// Project Context's framing explains how to read the records it carries; with
	// none carried the section holds only its placeholder, machine-owned text on
	// its own (ProtectedInference.cpp), and the explanation of records nobody
	// was given is not required prompt on every request.
	std::string project_context(kProjectContextPreamble);
	bool included_context = false;
	for (std::size_t index = 0; index < input.project_context_items.size(); ++index) {
		if (state.project_context_items[index] == false) continue;
		project_context += input.project_context_items[index].content;
		included_context = true;
	}
	if (included_context == false)
		project_context = input.project_context_items.empty() ? "(none supplied)" : std::string(excluded_text);
	append_section(prompt, "PROJECT_CONTEXT", project_context);

  std::string source(kSourceMaterialPreamble);
  bool included_source = false;
  // Each document's passages are read under its title, written once above the
  // run - again only if its passages resume after another document's.
  const std::string *headed = nullptr;
  for (std::size_t index = 0; index < sources.size(); ++index) {
    if (state.sources[index]) {
      const PromptSourceMaterial &passage = sources[index];
      if (passage.document_title.empty() || passage.document_label.empty()) {
        headed = nullptr;
      } else if (headed == nullptr || *headed != passage.document_id) {
        source += routing::source_material::document_header(passage.document_label, passage.document_title);
        headed = &passage.document_id;
      }
      source += passage.content;
      included_source = true;
    }
  }
  if (!included_source) {
    source += sources.empty() ? "(none supplied)" : excluded_text;
  }
  append_section(prompt, "SOURCE_MATERIAL", source);
  {
    std::string coverage;
    if (state.excluded_selected_sources > 0)
      coverage = source_coverage_text(sources, state, input.required_source_origin);
    const std::string relevance = relevance_coverage_text(input, sources, state);
    if (!relevance.empty()) coverage += (coverage.empty() ? "" : "\n") + relevance;
    if (!coverage.empty()) append_section(prompt, "SOURCE_COVERAGE", coverage);
  }
  if (input.temporal_context) {
    // Exactly the documents this rendering carries passages of.
    std::vector<std::string> carried;
    for (std::size_t index = 0; index < sources.size(); ++index)
      if (state.sources[index] && !sources[index].document_id.empty()
          && std::find(carried.begin(), carried.end(), sources[index].document_id) == carried.end())
        carried.push_back(sources[index].document_id);
    const std::string temporal = carried.size() >= 2 ? input.temporal_context(carried) : std::string();
    if (!temporal.empty()) append_section(prompt, "TEMPORAL_CONTEXT", temporal);
  }
  std::string conversation(kConversationStatePreamble);
  bool included_record = false;
  for (std::size_t index = 0; index < input.history_items.size(); ++index) {
    const std::string *record = rendered_turn_record(input.history_items[index], state, index);
    if (record == nullptr) continue;
    conversation += *record;
    if (conversation.back() != '\n') conversation.push_back('\n');
    included_record = true;
  }
  if (included_record) append_section(prompt, "CONVERSATION_STATE", conversation);
  std::string history(kHistoryPreamble);
  bool included_history = false;
  for (std::size_t index = 0; index < input.history_items.size(); ++index) {
    if (state.history_items[index]) {
      history += input.history_items[index].content;
      included_history = true;
    }
  }
  if (input.history && !blank(*input.history) && state.legacy_history) {
    history += *input.history;
    included_history = true;
  }
  if (!included_history) {
    history += (input.history_items.empty() &&
                (!input.history || blank(*input.history)))
                   ? "(none supplied)"
                   : std::string(excluded_text);
  }
  append_section(prompt, "HISTORY", history);
  const std::string coverage = request_coverage_text(input, state);
  if (!coverage.empty()) append_section(prompt, "REQUEST_COVERAGE", coverage);
  append_section(prompt, "CURRENT_REQUEST", input.current_request);
  return prompt;
}

nlohmann::json render_model_visible_data(const PromptAssemblyInput &input,
		const SystemPromptProfile &profile, const LegalTaskPreset &preset,
		const std::vector<PromptSourceMaterial> &sources, const RenderState &state) {
	nlohmann::json value{
		{"system", {
			{"profileId", std::string(profile.id)},
			{"profileVersion", profile.version},
			{"instruction", std::string(profile.instructions)},
		}},
		{"currentRequest", input.current_request},
		{"projectContext", nlohmann::json::array()},
		{"sourceMaterial", nlohmann::json::array()},
		{"history", nlohmann::json::array()},
	};
	if (state.task) {
		value["task"] = input.plain_chat
			? nlohmann::json{
				{"presetId", std::string(preset.id)},
				{"presetVersion", preset.version},
				{"name", "Ordinary conversation"},
				{"instruction", std::string(kPlainChatTaskText)},
				{"expectedOutputStyle", std::string(kPlainChatOutputStyle)},
			}
			: nlohmann::json{
				{"presetId", std::string(preset.id)},
				{"presetVersion", preset.version},
				{"name", std::string(preset.name)},
				{"instruction", std::string(preset.instructions)},
				{"expectedOutputStyle", std::string(preset.expected_output_style)},
			};
	}
	if (input.capability_instructions && !blank(*input.capability_instructions))
		value["capabilityInstruction"] = *input.capability_instructions;
	if (state.candidate_context && input.candidate_context && !blank(*input.candidate_context))
		value["candidateContext"] = *input.candidate_context;
	if (state.user_instructions)
		value["userDefaultInstruction"] = *input.user_default_instructions;
	if (state.matter_instructions)
		value["projectInstruction"] = *input.matter_instructions;
	for (std::size_t index = 0; index < input.project_context_items.size(); ++index) {
		if (!state.project_context_items[index]) continue;
		const auto &item = input.project_context_items[index];
		value["projectContext"].push_back({
			{"handle", item.id},
			{"contentClass", "project_context"},
			{"pinned", item.is_pinned},
			{"content", item.content},
		});
	}
	for (std::size_t index = 0; index < sources.size(); ++index) {
		if (!state.sources[index]) continue;
		const auto &source = sources[index];
		value["sourceMaterial"].push_back({
			{"handle", source.id},
			{"contentClass", "untrusted_evidence"},
			{"explicitlySelected", source.explicitly_selected},
			{"content", source.content},
		});
	}
	{
		std::string coverage;
		if (state.excluded_selected_sources > 0)
			coverage = source_coverage_text(sources, state, input.required_source_origin);
		const std::string relevance = relevance_coverage_text(input, sources, state);
		if (!relevance.empty()) coverage += (coverage.empty() ? "" : "\n") + relevance;
		if (!coverage.empty()) value["sourceCoverage"] = coverage;
	}
	for (std::size_t index = 0; index < input.history_items.size(); ++index) {
		const std::string *record = rendered_turn_record(input.history_items[index], state, index);
		if (record == nullptr) continue;
		if (!value.contains("conversationState")) value["conversationState"] = nlohmann::json::array();
		value["conversationState"].push_back({
			{"handle", input.history_items[index].id},
			{"contentClass", "prior_conversation_record"},
			{"content", *record},
		});
	}
	for (std::size_t index = 0; index < input.history_items.size(); ++index) {
		if (!state.history_items[index]) continue;
		value["history"].push_back({
			{"handle", input.history_items[index].id},
			{"contentClass", "prior_project_task"},
			{"content", input.history_items[index].content},
		});
	}
	if (const std::string coverage = request_coverage_text(input, state); !coverage.empty())
		value["requestCoverage"] = coverage;
	if (state.legacy_history) {
		value["history"].push_back({
			{"handle", "relevant-history"},
			{"contentClass", "prior_project_task"},
			{"content", *input.history},
		});
	}
	return value;
}

} // namespace

std::string PromptAssembler::assemble(const PromptAssemblyInput &input) {
  return assemble_with_budget(input).prompt;
}

PromptAssemblyResult
PromptAssembler::assemble_with_budget(const PromptAssemblyInput &input) {
  if (blank(input.current_request)) {
    throw std::runtime_error("Enter a task to run.");
  }

  const auto profile = find_system_prompt_profile(
      input.prompt_profile_id, input.prompt_profile_version);
  if (!profile) {
    throw std::runtime_error("Unknown Legal Machines prompt profile version.");
  }
  const auto preset = find_legal_task_preset(
      input.legal_task_preset_id, input.legal_task_preset_version);
  if (!preset) {
    throw std::runtime_error("Unknown legal task preset version.");
  }

  std::vector<PromptSourceMaterial> sources = input.source_chunks;
  if (input.source_material && !blank(*input.source_material)) {
    sources.push_back(
        {"legacy-source-material", *input.source_material, false});
  }

  RenderState state;
  state.sources.resize(sources.size(), false);
  state.history_items.resize(input.history_items.size(), false);
  state.history_records.resize(input.history_items.size(), false);
  state.history_briefs.resize(input.history_items.size(), false);
	state.project_context_items.resize(input.project_context_items.size(), false);
  PromptAssemblyResult result;
  result.budget.maximum_bytes = input.maximum_context_bytes;
  result.budget.included.push_back(
      {"system-security", input.prompt_profile_id, profile->instructions.size()});
  result.budget.included.push_back(
      {"current-request", "current-request", input.current_request.size()});
	if (input.capability_instructions && !blank(*input.capability_instructions))
		result.budget.included.push_back(
			{"capability", "professional-execution", input.capability_instructions->size()});

  // Known capacity is authoritative (see PromptAssemblyInput::
  // maximum_context_tokens): what the request requires fits it or the request
  // fails closed; only an unknown capacity falls back to the historical window.
  const bool capacity_known = input.maximum_context_tokens.has_value() && static_cast<bool>(input.estimate_tokens);
  result.budget.maximum_tokens = capacity_known ? input.maximum_context_tokens : std::nullopt;
  result.budget.capacity_known = capacity_known;
  auto prompt = render_prompt(input, *profile, *preset, sources, state);
  // The part of the output reservation that may yield to the prompt, and the
  // ceiling that leaves only the minimum output.
  const std::size_t yieldable_output_tokens = input.reserved_output_tokens > input.minimum_output_tokens
      ? input.reserved_output_tokens - input.minimum_output_tokens : 0;
  const std::size_t hard_token_ceiling = capacity_known ? *input.maximum_context_tokens + yieldable_output_tokens : 0;
  std::size_t optional_token_ceiling = 0;
  if (capacity_known == true) {
    const std::size_t required_tokens = input.estimate_tokens(prompt);
    if (required_tokens > hard_token_ceiling) {
      throw ContextCapacityExceeded(
          "This request is too large for the selected model's context window, even before any documents are "
          "added. Shorten the request or choose a model with a larger context window.");
    }
    if (required_tokens > *input.maximum_context_tokens) {
      // The required sections already need part of the output reservation:
      // everything else competes for what remains above the minimum output.
      optional_token_ceiling = hard_token_ceiling;
    } else {
      const std::size_t room = *input.maximum_context_tokens - required_tokens;
      optional_token_ceiling = *input.maximum_context_tokens - std::min(input.continuation_headroom_tokens, room / 2);
    }
  } else if (prompt.size() > input.maximum_context_bytes) {
    throw std::runtime_error(
        "The current request and required security instructions exceed the "
        "local context budget; the request was not truncated.");
  }
  const auto fits = [&](const std::string &candidate) {
    return capacity_known ? input.estimate_tokens(candidate) <= optional_token_ceiling
                          : candidate.size() <= input.maximum_context_bytes;
  };

  const auto consider = [&](std::string category, std::string id,
                            std::size_t content_bytes,
                            const std::function<void(RenderState &)> &include) {
    auto candidate_state = state;
    include(candidate_state);
    auto candidate_prompt =
        render_prompt(input, *profile, *preset, sources, candidate_state);
    PromptBudgetItem item{std::move(category), std::move(id), content_bytes};
    if (fits(candidate_prompt)) {
      state = std::move(candidate_state);
      prompt = std::move(candidate_prompt);
      result.budget.included.push_back(std::move(item));
    } else {
      result.budget.excluded.push_back(std::move(item));
    }
  };

  const auto task = task_text(*preset, input.plain_chat,
      input.candidate_context.has_value() && !blank(*input.candidate_context));
  consider("task-preset", std::string(preset->id), task.size(),
           [](RenderState &candidate) { candidate.task = true; });
  if (input.matter_instructions && !blank(*input.matter_instructions)) {
    consider("matter-instructions", "matter-instructions",
             input.matter_instructions->size(), [](RenderState &candidate) {
               candidate.matter_instructions = true;
             });
  }
  if (input.user_default_instructions &&
      !blank(*input.user_default_instructions)) {
    consider("user-instructions", "user-default-instructions",
             input.user_default_instructions->size(),
             [](RenderState &candidate) {
               candidate.user_instructions = true;
             });
  }

	const auto consider_context_items = [&](bool is_pinned) {
		for (std::size_t index = 0; index < input.project_context_items.size(); ++index) {
			if (input.project_context_items[index].is_pinned != is_pinned
					|| input.project_context_items[index].id.empty()
					|| blank(input.project_context_items[index].content)) {
				continue;
			}
			consider(is_pinned ? "pinned-project-context" : "project-context",
					input.project_context_items[index].id, input.project_context_items[index].content.size(),
					[index](RenderState &candidate) { candidate.project_context_items[index] = true; });
		}
	};
	consider_context_items(true);
	consider_context_items(false);

  // Includes `include`'s material when the whole prompt still fits.
  const auto try_include = [&](const std::function<void(RenderState &)> &include) {
    auto candidate_state = state;
    include(candidate_state);
    auto candidate_prompt = render_prompt(input, *profile, *preset, sources, candidate_state);
    if (!fits(candidate_prompt)) return false;
    state = std::move(candidate_state);
    prompt = std::move(candidate_prompt);
    return true;
  };
  // The most of `candidates` (newest last) that fit, newest first: the
  // largest count whose newest members fit, found by halving - each probe
  // renders the whole prompt once, however long the conversation.
  const auto include_newest = [&](const std::vector<std::size_t> &candidates,
                                  const std::function<void(RenderState &, std::size_t)> &mark) {
    std::size_t low = 0;
    std::size_t high = candidates.size();
    while (low < high) {
      const std::size_t count = (low + high + 1) / 2;
      const bool included = try_include([&](RenderState &candidate) {
        for (std::size_t offset = 0; offset < count; ++offset)
          mark(candidate, candidates[candidates.size() - 1 - offset]);
      });
      if (included) low = count;
      else high = count - 1;
    }
  };
  std::vector<std::size_t> turn_briefs;
  std::vector<std::size_t> turn_records;
  std::vector<std::size_t> whole_turns;
  for (std::size_t index = 0; index < input.history_items.size(); ++index) {
    if (!offered_turn(input.history_items[index])) continue;
    if (!blank(input.history_items[index].brief_record)) turn_briefs.push_back(index);
    if (!blank(input.history_items[index].state_record)) turn_records.push_back(index);
    if (!blank(input.history_items[index].content)) whole_turns.push_back(index);
  }
  // Every turn named briefly first, as far as capacity allows. Upgrading the
  // newest of them to their detailed records is a SECOND step, deliberately
  // separated: on a small window the upgrades used to take the whole budget
  // before the latest turn itself was considered, so a model answering "given
  // all that, what should we do next?" received thirty-five summaries and not
  // one word of the exchange it was answering. The last turn whole outranks a
  // fuller record of an older one.
  // Naming every turn is the cheapest thing the conversation can carry, and
  // where they do not all fit the ones that yield come from the MIDDLE.
  //
  // A conversation has two ends that matter: its opening, where a lawyer sets
  // the standing instruction ("we act for the buyer, answer in English") and
  // corrects a fact, and its recent turns, which are the live thread. Taking
  // the newest until the budget ran out dropped the opening first, so a
  // standing instruction given at turn one silently stopped applying at turn
  // thirty. Admitting from both ends inward keeps both, and what yields is the
  // middle - which is what a person summarising a long thread drops too.
  const auto include_turn_briefs = [&] {
    // A very long conversation is not walked turn by turn: the binary search
    // below costs one render per probe rather than one per turn.
    constexpr std::size_t kWalkedIndividuallyUpTo = 200;
    if (turn_briefs.size() > kWalkedIndividuallyUpTo) {
      include_newest(turn_briefs, [](RenderState &candidate, std::size_t index) {
        candidate.history_briefs[index] = true;
      });
      return;
    }
    std::size_t newest = turn_briefs.size();
    std::size_t oldest = 0;
    bool takeNewest = true;
    while (oldest < newest) {
      const std::size_t index = takeNewest ? turn_briefs[--newest] : turn_briefs[oldest++];
      takeNewest = !takeNewest;
      // A brief that does not fit is simply left out; the next one from the
      // other end may still fit, and both ends matter more than the middle.
      (void)try_include([index](RenderState &candidate) { candidate.history_briefs[index] = true; });
    }
  };
  const auto upgrade_newest_turns_to_records = [&] {
    include_newest(turn_records, [](RenderState &candidate, std::size_t index) {
      candidate.history_records[index] = true;
    });
  };
  const auto include_turn_records = [&] {
    include_turn_briefs();
    upgrade_newest_turns_to_records();
  };
  // The latest turn, whole, before anything else of the conversation.
  //
  // A follow-up is about the last exchange - "given all that, what should we do
  // next?" - so that exchange is the one piece the model cannot work without.
  // Naming every turn briefly used to come first and filled the budget to the
  // last token, after which the latest turn itself (a few dozen tokens) no
  // longer fit: the model received thirty-five summaries and not one word of
  // what it was answering. It is admitted first now; the briefs and then the
  // fuller records of the newest turns take what remains.
  const auto include_latest_turn_whole = [&] {
    if (whole_turns.empty()) return;
    const std::size_t latest = whole_turns.back();
    if (try_include([latest](RenderState &candidate) { candidate.history_items[latest] = true; }))
      whole_turns.pop_back();
    else
      whole_turns.clear();
  };
  include_latest_turn_whole();
  // Then the conversation's records, before evidence: a follow-up keeps what
  // was asked, decided and produced even when capacity is short. Every turn is
  // named here; the newest are upgraded to their full records afterwards.
  include_turn_briefs();

  // Passages are admitted round-robin across their documents (each
  // document's own passages by priority), measured incrementally - a large-
  // capacity prompt can hold thousands of passages, so no per-passage
  // re-render - and verified once against the exact rendering afterwards.
  const auto consider_sources = [&](bool explicitly_selected) {
    const char *category = explicitly_selected ? "explicit-source" : "retrieved-source";
    std::vector<std::vector<std::size_t>> groups;
    std::unordered_map<std::string, std::size_t> group_of_document;
    for (std::size_t index = 0; index < sources.size(); ++index) {
      if (sources[index].explicitly_selected != explicitly_selected) continue;
      const std::string &document = sources[index].document_id;
      const auto known = document.empty() ? group_of_document.end() : group_of_document.find(document);
      if (known == group_of_document.end()) {
        if (!document.empty()) group_of_document.emplace(document, groups.size());
        groups.push_back({index});
      } else {
        groups[known->second].push_back(index);
      }
    }
    if (groups.empty()) return;
    for (auto &group : groups)
      std::stable_sort(group.begin(), group.end(), [&](std::size_t left, std::size_t right) {
        return sources[left].priority < sources[right].priority;
      });
    // With no passage included yet, the rendering shows a placeholder that
    // the first admitted passage replaces.
    const bool placeholder_shown = std::find(state.sources.begin(), state.sources.end(), true) == state.sources.end();
    const std::size_t placeholder_bytes = escaped_section_bytes(excluded_text);
    const std::size_t base_bytes = prompt.size();
    const std::size_t base_tokens = capacity_known ? input.estimate_tokens(prompt) : 0;
    const std::size_t base_passages =
        static_cast<std::size_t>(std::count(state.sources.begin(), state.sources.end(), true));
    const std::size_t passage_window = capacity_known ? std::numeric_limits<std::size_t>::max()
                                                      : input.fallback_maximum_passages;
    std::size_t total_passages = 0;
    std::size_t total_bytes = 0;
    std::size_t total_tokens = 0;
    for (const auto &group : groups)
      for (const std::size_t index : group) {
        ++total_passages;
        total_bytes += escaped_section_bytes(sources[index].content);
        if (capacity_known) total_tokens += input.estimate_tokens(sources[index].content);
      }
    // A selection that cannot be delivered whole must say so: the worst-case
    // coverage statement is reserved before admission, so the exact
    // rendering stays within the bound.
    std::size_t coverage_bytes = 0;
    std::size_t coverage_tokens = 0;
    if (explicitly_selected) {
      std::string coverage;
      append_section(coverage, "SOURCE_COVERAGE", source_coverage_text(sources, state, input.required_source_origin, true));
      coverage_bytes = coverage.size();
      if (capacity_known) coverage_tokens = input.estimate_tokens(coverage);
    }
    const auto record_exclusions = [&] {
      if (!explicitly_selected) return;
      state.excluded_selected_sources = 0;
      for (const auto &group : groups)
        for (const std::size_t index : group)
          if (!state.sources[index]) ++state.excluded_selected_sources;
    };
    const auto exact_fits = [&](std::size_t token_ceiling) {
      return capacity_known ? input.estimate_tokens(prompt) <= token_ceiling
                            : prompt.size() <= input.maximum_context_bytes;
    };
    std::vector<std::size_t> admitted;
    const auto admit_within = [&](std::size_t token_ceiling) {
      const bool whole_fits = base_passages + total_passages <= passage_window
          && (capacity_known ? base_tokens + total_tokens <= token_ceiling
                             : base_bytes + total_bytes - (placeholder_shown ? placeholder_bytes : 0)
                                   <= input.maximum_context_bytes);
      const std::size_t reserved_bytes = whole_fits ? 0 : coverage_bytes;
      const std::size_t reserved_tokens = whole_fits ? 0 : coverage_tokens;
      bool placeholder_replaced = !placeholder_shown;
      std::size_t prompt_bytes = base_bytes;
      std::size_t prompt_tokens = base_tokens;
      std::size_t admitted_passages = base_passages;
      for (std::size_t round = 0;; ++round) {
        bool offered = false;
        for (const auto &group : groups) {
          if (round >= group.size()) continue;
          offered = true;
          const std::size_t index = group[round];
          const std::size_t next_bytes = prompt_bytes + escaped_section_bytes(sources[index].content)
              - (placeholder_replaced ? 0 : placeholder_bytes);
          const std::size_t next_tokens =
              capacity_known ? prompt_tokens + input.estimate_tokens(sources[index].content) : 0;
          if (admitted_passages >= passage_window) continue;
          if (capacity_known ? next_tokens + reserved_tokens > token_ceiling
                             : next_bytes + reserved_bytes > input.maximum_context_bytes)
            continue;
          state.sources[index] = true;
          prompt_bytes = next_bytes;
          prompt_tokens = next_tokens;
          placeholder_replaced = true;
          ++admitted_passages;
          admitted.push_back(index);
        }
        if (!offered) break;
      }
      record_exclusions();
      prompt = render_prompt(input, *profile, *preset, sources, state);
      // Exact verification: a boundary token spanning two passages can only
      // make the rendering larger than measured - withdraw the most recently
      // admitted passages until the exact rendering fits.
      while (!admitted.empty() && !exact_fits(token_ceiling)) {
        state.sources[admitted.back()] = false;
        admitted.pop_back();
        record_exclusions();
        prompt = render_prompt(input, *profile, *preset, sources, state);
      }
    };
    admit_within(optional_token_ceiling);
    if (explicitly_selected && capacity_known) {
      // The continuation headroom, and then the output reservation down to its
      // minimum, yield before a request loses its only admissible selected
      // evidence. Known capacity never proceeds without that evidence.
      if (admitted.empty() && optional_token_ceiling < *input.maximum_context_tokens)
        admit_within(*input.maximum_context_tokens);
      if (admitted.empty() && optional_token_ceiling < hard_token_ceiling && *input.maximum_context_tokens < hard_token_ceiling)
        admit_within(hard_token_ceiling);
      if (admitted.empty()) {
        throw ContextCapacityExceeded(input.required_source_origin == RequiredSourceOrigin::UserSelection
            ? "The selected documents do not fit the selected model's context window together with this request. "
              "Select fewer or smaller documents, shorten the request or choose a model with a larger context "
              "window."
            : "The documents this request must re-read do not fit the selected model's context window together "
              "with this request. Choose a model with a larger context window.");
      }
    } else if (explicitly_selected && (admitted.empty() || !exact_fits(0))) {
      // Unknown capacity: the historical window never proceeds without any of
      // the required passages either, and the refusal names the real cause -
      // the instructions and Project Context admitted before them leave no
      // room - rather than blaming the request text.
      throw ContextCapacityExceeded(input.required_source_origin == RequiredSourceOrigin::UserSelection
          ? "The selected documents do not fit the context budget left beside this request's instructions and "
            "Project Context, so nothing was sent. Select fewer or smaller documents or reduce the pinned Project "
            "Context."
          : "The documents this request must re-read do not fit the context budget left beside this request's "
            "instructions and Project Context, so nothing was sent.");
    }
    for (const auto &group : groups)
      for (const std::size_t index : group)
        (state.sources[index] ? result.budget.included : result.budget.excluded)
            .push_back({category, sources[index].id, sources[index].content.size()});
  };
  try {
    consider_sources(true);
  } catch (const ContextCapacityExceeded &) {
    // An explicit selection outranks the conversation: it yields WHOLE - the
    // latest turn included, since that is now admitted before the sources are
    // considered - and returns afterwards only as far as room remains. Leaving
    // the latest turn in place here would refuse a selection the lawyer made
    // because of a turn Singularity chose to carry.
    const bool conversationCanYield =
        std::find(state.history_records.begin(), state.history_records.end(), true) != state.history_records.end()
        || std::find(state.history_briefs.begin(), state.history_briefs.end(), true) != state.history_briefs.end()
        || std::find(state.history_items.begin(), state.history_items.end(), true) != state.history_items.end();
    if (!conversationCanYield) throw;
    std::fill(state.history_records.begin(), state.history_records.end(), false);
    std::fill(state.history_briefs.begin(), state.history_briefs.end(), false);
    std::fill(state.history_items.begin(), state.history_items.end(), false);
    whole_turns.clear();
    for (std::size_t index = 0; index < input.history_items.size(); ++index)
      if (!blank(input.history_items[index].content)) whole_turns.push_back(index);
    for (std::size_t index = 0; index < sources.size(); ++index)
      if (sources[index].explicitly_selected) state.sources[index] = false;
    state.excluded_selected_sources = 0;
    prompt = render_prompt(input, *profile, *preset, sources, state);
    consider_sources(true);
    include_latest_turn_whole();
    include_turn_records();
  }
  // The newest turns' fuller records, once the latest turn itself and every
  // turn's brief are in: a fuller record of an older turn never costs the
  // exchange the request is actually about.
  upgrade_newest_turns_to_records();
  consider_sources(false);
	// The bound candidate's preview comes after the evidence: a bounded reading
	// of what a change could target takes only the room the sources leave, and
	// is left out before a selected document ever is.
	if (input.candidate_context && !blank(*input.candidate_context)) {
		consider("candidate-context", "resolved-candidates", input.candidate_context->size(),
			[](RenderState &candidate) { candidate.candidate_context = true; });
	}
  // Older whole turns, newest first: the whole turns stay the most recent ones.
  include_newest(whole_turns, [](RenderState &candidate, std::size_t index) {
    candidate.history_items[index] = true;
  });
  for (std::size_t index = 0; index < input.history_items.size(); ++index) {
    const PromptHistoryItem &item = input.history_items[index];
    if (!offered_turn(item)) continue;
    const bool whole = state.history_items[index];
    const std::string *record = rendered_turn_record(item, state, index);
    if (!blank(item.content))
      (whole ? result.budget.included : result.budget.excluded).push_back({"history", item.id, item.content.size()});
    if (!whole && record != nullptr)
      result.budget.included.push_back({"conversation-record", item.id, record->size()});
    else if (!whole && (!blank(item.state_record) || !blank(item.brief_record)))
      result.budget.excluded.push_back({"conversation-record", item.id,
          std::max(item.state_record.size(), item.brief_record.size())});
  }
  if (input.history && !blank(*input.history)) {
    consider("history", "relevant-history", input.history->size(),
             [](RenderState &candidate) { candidate.legacy_history = true; });
  }

  result.prompt = std::move(prompt);
  result.model_visible_data = render_model_visible_data(
		input, *profile, *preset, sources, state);
  result.budget.assembled_bytes = result.prompt.size();
  result.budget.assembled_tokens = capacity_known ? input.estimate_tokens(result.prompt) : 0;
  if (capacity_known && input.reserved_output_tokens > 0) {
    const std::size_t yielded = result.budget.assembled_tokens > *input.maximum_context_tokens
        ? std::min(result.budget.assembled_tokens - *input.maximum_context_tokens, yieldable_output_tokens) : 0;
    result.budget.output_tokens = input.reserved_output_tokens - yielded;
  }
  result.budget.excluded_selected_sources = state.excluded_selected_sources;
  PromptCoverage &coverage = result.budget.coverage;
  coverage.relevance_candidate_documents = input.relevance_candidate_documents;
  {
    std::unordered_map<std::string, std::size_t> position_of_document;
    for (std::size_t index = 0; index < sources.size(); ++index) {
      const std::string key = sources[index].document_id.empty()
          ? "#" + std::to_string(index) : sources[index].document_id;
      auto position = position_of_document.find(key);
      if (position == position_of_document.end()) {
        position = position_of_document.emplace(key, coverage.sources.size()).first;
        PromptSourceCoverage entry;
        entry.document_id = sources[index].document_id;
        entry.label = sources[index].document_label;
        entry.explicitly_selected = sources[index].explicitly_selected;
        coverage.sources.push_back(std::move(entry));
      }
      PromptSourceCoverage &entry = coverage.sources[position->second];
      ++entry.retrieved;
      if (state.sources[index]) ++entry.supplied;
      if (!sources[index].explicitly_selected) coverage.relevance_selection = true;
    }
  }
  for (std::size_t index = 0; index < input.project_context_items.size(); ++index) {
    if (!offered_context_item(input.project_context_items[index])) continue;
    ++coverage.context_items;
    if (!state.project_context_items[index])
      coverage.omitted_context_item_ids.push_back(input.project_context_items[index].id);
  }
  for (std::size_t index = 0; index < input.history_items.size(); ++index) {
    if (!offered_turn(input.history_items[index])) continue;
    ++coverage.conversation_turns;
    const std::string &id = input.history_items[index].id;
    if (state.history_items[index]) coverage.turns_in_full.push_back(id);
    else if (rendered_turn_record(input.history_items[index], state, index) != nullptr) coverage.turns_as_records.push_back(id);
    else coverage.omitted_turns.push_back(id);
  }
  return result;
}

} // namespace singularity::context
