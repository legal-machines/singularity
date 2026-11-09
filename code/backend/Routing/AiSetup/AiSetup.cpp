#include "Routing/AiSetup/AiSetup.hpp"

#include <stdexcept>

#include <algorithm>

namespace singularity::routing {

const char *to_string(AiSetupRole role) {
  switch (role) {
    case AiSetupRole::Reasoning: return "reasoning";
    case AiSetupRole::Anonymization: return "anonymization";
    case AiSetupRole::SemanticSearch: return "semanticSearch";
    case AiSetupRole::Context: return "context";
    case AiSetupRole::Drafting: return "drafting";
  }
  return "reasoning";
}

const char *display_label(AiSetupRole role) {
  switch (role) {
    case AiSetupRole::Reasoning: return "Reasoning";
    case AiSetupRole::Anonymization: return "Anonymization";
    case AiSetupRole::SemanticSearch: return "Semantic search";
    case AiSetupRole::Context: return "Context";
    case AiSetupRole::Drafting: return "Drafting";
  }
  return "Reasoning";
}

std::optional<AiSetupRole> ai_setup_role_from_string(const std::string &value) {
  for (const auto role : all_ai_setup_roles()) {
    if (value == to_string(role)) return role;
  }
  return std::nullopt;
}

std::vector<RoleAssignmentEntry> accept_assignment_set(const std::vector<RoleAssignmentEntry> &previous,
    const std::vector<RoleAssignmentEntry> &next,
    const std::function<void(AiSetupRole, const ModelTarget &)> &validate) {
  std::vector<RoleAssignmentEntry> kept;
  for (const RoleAssignmentEntry &entry : next) {
    const std::optional<AiSetupRole> role = ai_setup_role_from_string(entry.role);
    if (!role) throw std::runtime_error("This is not a real AI Setup role.");
    // Kept exactly as it was: not this save's choice, so not checked again.
    const bool unchanged = std::any_of(previous.begin(), previous.end(), [&](const RoleAssignmentEntry &before) {
      return before.role == entry.role && before.provider_id == entry.provider_id && before.model_id == entry.model_id;
    });
    if (!is_role_assignable(*role)) {
      // An entry saved before the role became Singularity's own is dropped;
      // a new choice for it is refused, with the reason.
      if (unchanged) continue;
      throw std::runtime_error(fixed_role_reason(*role));
    }
    if (!unchanged) validate(*role, ModelTarget{entry.provider_id, entry.model_id});
    kept.push_back(entry);
  }
  return kept;
}

bool is_role_assignable(AiSetupRole role) {
	// Anonymization is filled by Singularity's own bundled Papinian package. It is
	// the only role whose assignment the runtime never consults, so it is the
	// only one reported as fixed (see is_role_assignable()'s own comment).
	return role != AiSetupRole::Anonymization;
}

const char *fixed_role_reason(AiSetupRole role) {
	if (role == AiSetupRole::Anonymization)
		return "Singularity protects your data with its own built-in Papinian detection, on this computer. "
			"This is not a choice, so nothing here can weaken it.";
	return "";
}

const std::vector<AiSetupRole> &all_ai_setup_roles() {
  static const std::vector<AiSetupRole> roles{
      AiSetupRole::Reasoning, AiSetupRole::Anonymization, AiSetupRole::SemanticSearch,
      AiSetupRole::Context, AiSetupRole::Drafting,
  };
  return roles;
}

const char *to_string(AiSetupSource source) {
  switch (source) {
    case AiSetupSource::NodeDefault: return "nodeDefault";
    case AiSetupSource::ProjectOverride: return "projectOverride";
    case AiSetupSource::ChatOverride: return "chatOverride";
    case AiSetupSource::OperationOverride: return "operationOverride";
    case AiSetupSource::Unassigned: return "unassigned";
  }
  return "unassigned";
}

std::optional<ModelTarget> node_default_reasoning_target(const platform::LocalNodeSetup &setup) {
  const auto reasoning = std::find_if(setup.role_assignments.begin(), setup.role_assignments.end(),
      [](const platform::RoleModelAssignment &assignment) { return assignment.role == "reasoning"; });
  if (reasoning == setup.role_assignments.end()) return std::nullopt;
  return ModelTarget{reasoning->provider_id, reasoning->model_id};
}

namespace {

std::optional<ModelTarget> find_in(const AiSetupOverrideSet &overrides, AiSetupRole role) {
  const auto found = std::find_if(overrides.begin(), overrides.end(),
      [&](const RoleAssignment &assignment) { return assignment.role == role; });
  return found == overrides.end() ? std::nullopt : std::optional<ModelTarget>(found->model);
}

} // namespace

ResolvedRoleAssignment resolve_role(AiSetupRole role,
                                    const AiSetupOverrideSet &node_default,
                                    const AiSetupOverrideSet &project_override,
                                    const AiSetupOverrideSet &chat_override,
                                    const AiSetupOverrideSet &operation_override) {
  if (const auto operation_model = find_in(operation_override, role)) {
    return ResolvedRoleAssignment{role, operation_model, AiSetupSource::OperationOverride};
  }
  if (const auto chat_model = find_in(chat_override, role)) {
    return ResolvedRoleAssignment{role, chat_model, AiSetupSource::ChatOverride};
  }
  if (const auto project_model = find_in(project_override, role)) {
    return ResolvedRoleAssignment{role, project_model, AiSetupSource::ProjectOverride};
  }
  if (const auto node_model = find_in(node_default, role)) {
    return ResolvedRoleAssignment{role, node_model, AiSetupSource::NodeDefault};
  }
  return ResolvedRoleAssignment{role, std::nullopt, AiSetupSource::Unassigned};
}

std::vector<ResolvedRoleAssignment> resolve_effective_setup(
    const AiSetupOverrideSet &node_default,
    const AiSetupOverrideSet &project_override,
    const AiSetupOverrideSet &chat_override,
    const AiSetupOverrideSet &operation_override) {
  std::vector<ResolvedRoleAssignment> resolved;
  resolved.reserve(all_ai_setup_roles().size());
  for (const auto role : all_ai_setup_roles()) {
    resolved.push_back(resolve_role(role, node_default, project_override, chat_override, operation_override));
  }
  return resolved;
}

std::vector<std::string> compatible_local_families(AiSetupRole role) {
  if (role == AiSetupRole::Anonymization) return {"Papinian"};
  if (role == AiSetupRole::SemanticSearch) return {"Modestin"};
  return {};
}

bool requires_local_family(AiSetupRole role) {
  return !compatible_local_families(role).empty();
}

bool is_local_model_compatible(AiSetupRole role, const std::string &family,
                               hardware::ModelArchitectureKind architecture_kind) {
  const auto families = compatible_local_families(role);
  if (!families.empty()) {
    return std::find(families.begin(), families.end(), family) != families.end();
  }
  // A generic text role (Reasoning/Context/Drafting) needs a real generative
  // model - never Papinian's own non-generative extraction encoder, which
  // registers under architecture_kind == EncoderExtraction precisely because
  // it cannot produce free text at all.
  return architecture_kind == hardware::ModelArchitectureKind::DenseTransformer
      || architecture_kind == hardware::ModelArchitectureKind::MixtureOfExperts;
}

bool is_provider_model_compatible(AiSetupRole role, bool text_capable) {
  if (requires_local_family(role)) return false;
  return text_capable;
}

namespace {

const TaskProfile &conversational_reasoning_profile() {
  // Reasoning is the only role a normal conversational request always
  // invokes. Anonymization/Semantic search/Context are supporting, not
  // required - they participate only when the concrete request actually
  // needs them (an active protection policy, selected sources, enabled
  // Context), which this static profile does not itself know.
  static const TaskProfile profile{
      {AiSetupRole::Reasoning},
      {AiSetupRole::Anonymization, AiSetupRole::SemanticSearch, AiSetupRole::Context},
      AiSetupRole::Reasoning,
  };
  return profile;
}

const TaskProfile &drafting_profile() {
  // Drafting is task-specific - it never runs automatically after every
  // Reasoning operation, and this profile is only ever selected for a real
  // drafting stage, not a normal conversational one.
  static const TaskProfile profile{
      {AiSetupRole::Drafting},
      {AiSetupRole::Anonymization, AiSetupRole::Context},
      AiSetupRole::Drafting,
  };
  return profile;
}

} // namespace

const char *to_string(AiOperationKind kind) {
  switch (kind) {
    case AiOperationKind::ConversationalReasoning: return "conversationalReasoning";
    case AiOperationKind::Drafting: return "drafting";
  }
  return "conversationalReasoning";
}

const TaskProfile &task_profile_for(AiOperationKind kind) {
  switch (kind) {
    case AiOperationKind::ConversationalReasoning: return conversational_reasoning_profile();
    case AiOperationKind::Drafting: return drafting_profile();
  }
  return conversational_reasoning_profile();
}

AiOperationKind operation_kind_for_legal_task_preset(std::string_view legal_task_preset_id) {
  if (legal_task_preset_id == "draft") return AiOperationKind::Drafting;
  return AiOperationKind::ConversationalReasoning;
}

std::vector<AiSetupRole> roles_assigned_to(const AiSetupOverrideSet& overrides, const ModelTarget& target) {
	std::vector<AiSetupRole> roles;
	for (const auto& assignment : overrides) {
		if (assignment.model.provider_id == target.provider_id && assignment.model.model_id == target.model_id) {
			roles.push_back(assignment.role);
		}
	}
	return roles;
}

} // namespace singularity::routing
