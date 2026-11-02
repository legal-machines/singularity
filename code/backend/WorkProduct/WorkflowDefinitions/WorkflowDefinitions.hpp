#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace singularity::context {

// Master Spec Sec.69 (Completor): "A step references: preset, instructions,
// source policy." An Inference step runs through the exact same secure task
// pipeline a single matter.runTask call does (see execute_task_attempt() in
// native_bridge.cpp) - `preset_id`/`preset_version` name which
// legal_task_presets() entry supplies its system-level task instructions,
// `instructions` is this step's own concrete task prompt (what a lawyer
// would have typed for this one step). A CreateIssuesFromFindings step is
// not an inference step at all - no preset, no instructions, no model call
// - it deterministically promotes the Findings a prior step in the same
// workflow already parsed into real Issues via the existing
// matters::promote_finding_to_issue() path (Sec.26), exactly as "Add to
// issues" already does today. This is Completor's only non-inference step
// kind: "source policy" for every Inference step in V1 is the run's own
// caller-supplied selected-documents set (see run_workflow() in
// native_bridge.cpp), applied identically to each step rather than
// duplicated per step - the same selectedDocumentIds contract
// matter.runTask already exposes, not a second policy concept.
enum class WorkflowStepKind { Inference, CreateIssuesFromFindings };

struct WorkflowStepDefinition {
  std::string id;
  std::string name;
  WorkflowStepKind kind = WorkflowStepKind::Inference;
  // Only meaningful when kind == Inference.
  std::string preset_id;
  int preset_version = 1;
  std::string instructions;
  // Only set when kind == CreateIssuesFromFindings: the id of this same
  // workflow's own earlier step whose parsed Findings should be promoted.
  std::optional<std::string> findings_source_step_id;
};

struct WorkflowDefinition {
  std::string id;
  std::string name;
  int version;
  std::vector<WorkflowStepDefinition> steps;
};

// The provider-neutral, versioned registry used by Completor (Master Spec
// Sec.69). IDs/versions are persistence/API values and therefore stable -
// same convention as context::legal_task_presets().
const std::vector<WorkflowDefinition> &workflow_definitions();

// Returns no value for an unknown ID or a version that is no longer
// present - mirrors context::find_legal_task_preset()'s fail-closed
// contract. Callers must not silently substitute a different workflow for
// an unrecognized one.
std::optional<WorkflowDefinition> find_workflow_definition(std::string_view id, int version);

} // namespace singularity::context
