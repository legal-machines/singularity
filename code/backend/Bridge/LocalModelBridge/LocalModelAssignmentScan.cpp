#include "Bridge/LocalModelBridge/LocalModelAssignmentScan.hpp"

#include "Routing/AiSetup/AiSetup.hpp"

namespace singularity::bridge::local_model_bridge {
namespace {

void collect_node_style(const std::vector<platform::RoleModelAssignment>& node_style,
	const std::string& scope_description, const routing::ModelTarget& target,
	std::vector<ModelAssignmentReference>& references) {
	routing::AiSetupOverrideSet overrides;
	for (const auto& entry : node_style) {
		if (const auto role = routing::ai_setup_role_from_string(entry.role)) {
			overrides.push_back(routing::RoleAssignment{*role, routing::ModelTarget{entry.provider_id, entry.model_id}});
		}
	}
	for (const auto role : routing::roles_assigned_to(overrides, target)) {
		references.push_back(ModelAssignmentReference{role, scope_description});
	}
}

void collect_chat_style(const std::vector<matters::AiSetupRoleOverride>& chat_style,
	const std::string& scope_description, const routing::ModelTarget& target,
	std::vector<ModelAssignmentReference>& references) {
	routing::AiSetupOverrideSet overrides;
	for (const auto& entry : chat_style) {
		if (const auto role = routing::ai_setup_role_from_string(entry.role)) {
			overrides.push_back(routing::RoleAssignment{*role, routing::ModelTarget{entry.provider_id, entry.model_id}});
		}
	}
	for (const auto role : routing::roles_assigned_to(overrides, target)) {
		references.push_back(ModelAssignmentReference{role, scope_description});
	}
}

} // namespace

AssignmentScanResult gather_assignment_references(
	const platform::LocalNodeSetup& setup,
	const routing::ModelTarget& target,
	const std::function<std::vector<matters::Matter>(const std::filesystem::path&)>& load_matters_for_workspace,
	const std::function<bool(const std::filesystem::path&)>& is_workspace_available) {

	AssignmentScanResult result;
	collect_node_style(setup.role_assignments, "", target, result.references);

	// Every registered workspace is scanned by the same rule - the active
	// one is not special. A workspace that cannot be inspected right now is
	// recorded as uninspectable, never silently treated as empty.
	for (const auto& workspace : setup.workspaces) {
		if (!is_workspace_available(workspace.path)) {
			result.uninspectable_workspace_names.push_back(workspace.display_name);
			continue;
		}
		for (const auto& matter : load_matters_for_workspace(workspace.path)) {
			collect_chat_style(matter.ai_setup_overrides, "the Project \"" + matter.name + "\"", target, result.references);
			for (const auto& chat : matter.chats) {
				collect_chat_style(chat.ai_setup_overrides, "a Chat in the Project \"" + matter.name + "\"", target,
					result.references);
			}
		}
	}
	return result;
}

} // namespace singularity::bridge::local_model_bridge
