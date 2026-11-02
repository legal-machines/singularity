#include "Routing/LegalTaskPresets/LegalTaskPresets.hpp"

#include <algorithm>

namespace singularity::context {

const std::vector<LegalTaskPreset> &legal_task_presets() {
  static const std::vector<LegalTaskPreset> presets{
      {"general", 1, "General",
       "Complete the legal task requested by the user using only the supplied context where the task is source-bound.",
       "A direct, professionally written answer appropriate to the request."},
      {"summarize", 1, "Summarize",
       "Summarize the material facts, operative provisions and material qualifications in the supplied source material. Do not add facts that are not present.",
       "A concise structured summary that separates key points from qualifications."},
      {"risk-review", 1, "Risk Review",
       "Identify material legal and commercial risks in the supplied source material. For each material issue, identify the relevant source, explain the issue and consequence, distinguish source fact from inference, and do not invent missing provisions.",
       "A prioritized issue list with source, consequence and recommended follow-up for each item.",
       professional::StructuredResultKind::Findings},
      {"risk-review", 2, "Risk Review",
       "Produce a prioritized risk register, not a general summary. For every material risk, identify the supporting source text, distinguish document fact from legal or commercial inference, state the consequence for the represented client, and give a concrete recommended follow-up. Do not invent missing provisions.",
       "A risk register ordered by materiality with columns for severity (High, Medium or Low), source fact, client consequence and recommended follow-up.",
       professional::StructuredResultKind::Findings},
      {"compare-documents", 1, "Compare Documents",
       "Identify substantive differences, inconsistencies, omissions and changed positions across the selected materials. Identify the relevant source for each comparison.",
       "A structured comparison grouped by topic, including materiality and source references.",
       professional::StructuredResultKind::Changes},
      {"extract-obligations", 1, "Extract Obligations",
       "Extract express obligations from the supplied source material without inventing implied duties. Preserve relevant conditions, deadlines, responsible parties and exceptions.",
       "A structured obligations list with party, action, timing, conditions and source.",
       professional::StructuredResultKind::Obligations},
      {"draft", 1, "Draft",
       "Prepare the requested legal draft using supplied facts and instructions. Mark assumptions and unresolved inputs, and do not fabricate clauses or facts represented as source-derived.",
       "Professional legal drafting followed by a short assumptions and open-points section where needed."},
      {"due-diligence-review", 1, "Due Diligence Review",
       "Identify material findings, missing information and follow-up points from the selected Matter documents. Distinguish confirmed findings from unresolved diligence questions.",
       "A materiality-ordered findings table with evidence, impact and follow-up.",
       professional::StructuredResultKind::Findings},
      {"negotiation-points", 1, "Negotiation Points",
       "Identify positions that may require negotiation, explain their legal or commercial effect, and propose precise fallback positions consistent with the supplied instructions.",
       "A prioritized negotiation list with current position, concern, proposed position and fallback."},
      {"chronology", 1, "Chronology",
       "Build a chronology only from dates and events supported by the supplied source material. Flag ambiguous, conflicting or missing dates rather than resolving them by guesswork.",
       "A date-ordered chronology with event, parties, source and uncertainty notes.",
       professional::StructuredResultKind::Events},
  };
  return presets;
}

std::optional<LegalTaskPreset> find_legal_task_preset(
    std::string_view id, int version) {
  const auto &presets = legal_task_presets();
  const auto found = std::find_if(
      presets.begin(), presets.end(), [&](const LegalTaskPreset &preset) {
        return preset.id == id && preset.version == version;
      });
  return found == presets.end() ? std::nullopt
                                : std::optional<LegalTaskPreset>(*found);
}

const std::vector<LegalTaskPreset> &selectable_legal_task_presets() {
  static const std::vector<LegalTaskPreset> selectable = [] {
    std::vector<LegalTaskPreset> result;
    for (const auto &preset : legal_task_presets()) {
      if (preset.id == kDefaultLegalTaskPresetId) continue;
      const auto existing = std::find_if(result.begin(), result.end(),
          [&](const LegalTaskPreset &entry) { return entry.id == preset.id; });
      if (existing == result.end()) {
        result.push_back(preset);
      } else if (preset.version > existing->version) {
        *existing = preset;
      }
    }
    return result;
  }();
  return selectable;
}

} // namespace singularity::context
