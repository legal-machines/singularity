#include "WorkProduct/WorkflowDefinitions/WorkflowDefinitions.hpp"

#include <algorithm>

namespace singularity::context {

const std::vector<WorkflowDefinition> &workflow_definitions() {
  // Master Spec Sec.69's own two named workflows. Every Inference step maps
  // onto an already-real, already-tuned legal_task_presets() entry - Sec.69
  // asks for no capability this phase would need to invent a new preset
  // for. "Create issues" (Due Diligence Review's step 4) is deliberately
  // not an inference step - see WorkflowStepKind::CreateIssuesFromFindings.
  static const std::vector<WorkflowDefinition> workflows{
      {"contract-review", "Contract Review", 1,
       {
           {"summarize-transaction", "Summarize transaction/document",
            WorkflowStepKind::Inference, "summarize", 1,
            "Summarize the transaction and the documents in this Matter.",
            std::nullopt},
           {"extract-obligations", "Extract obligations",
            WorkflowStepKind::Inference, "extract-obligations", 1,
            "Extract the express obligations from the documents in this Matter.",
            std::nullopt},
           {"identify-risks", "Identify legal/commercial risks",
            WorkflowStepKind::Inference, "risk-review", 2,
            "Identify the material legal and commercial risks in this Matter.",
            std::nullopt},
           {"identify-negotiation-points", "Identify negotiation points",
            WorkflowStepKind::Inference, "negotiation-points", 1,
            "Identify the positions in this Matter that may require negotiation.",
            std::nullopt},
           {"draft-review-notes", "Draft Matter review notes",
            WorkflowStepKind::Inference, "draft", 1,
            "Draft Matter review notes summarizing the transaction, obligations, risks and "
            "negotiation points identified above for this Matter.",
            std::nullopt},
       }},
      {"due-diligence-review", "Due Diligence Review", 1,
       {
           {"inventory-documents", "Inventory selected documents",
            WorkflowStepKind::Inference, "general", 1,
            "Produce an inventory of the documents selected for this Matter, identifying each "
            "document's type and its role in the diligence review.",
            std::nullopt},
           {"identify-findings", "Identify material findings",
            WorkflowStepKind::Inference, "due-diligence-review", 1,
            "Identify the material findings in the documents selected for this Matter.",
            std::nullopt},
           {"identify-missing-documentation", "Identify missing documentation",
            WorkflowStepKind::Inference, "due-diligence-review", 1,
            "Identify documentation that appears to be missing or incomplete based on the "
            "documents selected for this Matter.",
            std::nullopt},
           {"create-issues", "Create issues",
            WorkflowStepKind::CreateIssuesFromFindings, "", 1, "",
            "identify-findings"},
           {"draft-dd-summary", "Draft DD summary",
            WorkflowStepKind::Inference, "draft", 1,
            "Draft a due diligence summary covering the document inventory, material findings, "
            "missing documentation and issues identified above for this Matter.",
            std::nullopt},
       }},
  };
  return workflows;
}

std::optional<WorkflowDefinition> find_workflow_definition(std::string_view id, int version) {
  const auto &workflows = workflow_definitions();
  const auto found = std::find_if(
      workflows.begin(), workflows.end(), [&](const WorkflowDefinition &workflow) {
        return workflow.id == id && workflow.version == version;
      });
  return found == workflows.end() ? std::nullopt : std::optional<WorkflowDefinition>(*found);
}

} // namespace singularity::context
