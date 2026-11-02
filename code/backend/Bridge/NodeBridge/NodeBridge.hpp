#pragma once

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>

namespace singularity::bridge::node_bridge {

nlohmann::json get_summary(const nlohmann::json &params);
nlohmann::json check_legal_machines_connection(const nlohmann::json &params);
// The emergency "severe-state memory release pass" - see
// LocalModel/LocalModelRegistry/LocalModelRegistry.hpp's release_reclaimable_memory().
// Never touches Project/.lm state; only ever stops non-required local
// models. Returns the refreshed node summary.
nlohmann::json release_reclaimable_memory(const nlohmann::json &params);
nlohmann::json get_onboarding_status(const nlohmann::json &params);
nlohmann::json complete_onboarding(const nlohmann::json &params);
nlohmann::json save_account_resume(const nlohmann::json &params);
nlohmann::json save_local_setup(const nlohmann::json &params);
nlohmann::json get_legal_instructions(const nlohmann::json &params);
nlohmann::json save_person(const nlohmann::json &params);
nlohmann::json save_legal_instruction_settings(const nlohmann::json &params);
nlohmann::json get_dismissed_notice_ids(const nlohmann::json &params);
nlohmann::json dismiss_notice(const nlohmann::json &params);
nlohmann::json list_workspaces(const nlohmann::json &params);
nlohmann::json choose_workspace(const std::optional<std::string> &selected_path);
// Records access to a folder the user chose (Settings > Permissions) without
// changing the active workspace that holds the Project registry.
nlohmann::json grant_folder_access(const std::string &selected_path);
nlohmann::json remove_workspace(const nlohmann::json &params);
nlohmann::json set_active_workspace(const nlohmann::json &params);
void validate_relocation(const nlohmann::json &params);
nlohmann::json relocate_workspace(const nlohmann::json &params,
                                  const std::optional<std::string> &selected_path);

} // namespace singularity::bridge::node_bridge
