#pragma once

#include <nlohmann/json_fwd.hpp>

namespace singularity::bridge::project_context_bridge {

nlohmann::json list_items(const nlohmann::json &params);
nlohmann::json list_suggestions(const nlohmann::json &params);
nlohmann::json search_items(const nlohmann::json &params);
nlohmann::json get_page_state(const nlohmann::json &params);
nlohmann::json run_automatic_update(const nlohmann::json &params);
nlohmann::json accept_suggestion(const nlohmann::json &params);
nlohmann::json dismiss_suggestion(const nlohmann::json &params);
nlohmann::json create_item(const nlohmann::json &params);
nlohmann::json memory_chat(const nlohmann::json &params);
nlohmann::json memory_status(const nlohmann::json &params);
nlohmann::json memory_read_documents(const nlohmann::json &params);
nlohmann::json create_item_from_chat_result(const nlohmann::json &params);
nlohmann::json update_item(const nlohmann::json &params);
nlohmann::json delete_item(const nlohmann::json &params);
nlohmann::json link_item_entity(const nlohmann::json &params);
nlohmann::json unlink_item_entity(const nlohmann::json &params);
nlohmann::json update_item_controls(const nlohmann::json &params);
nlohmann::json list_merge_conflicts(const nlohmann::json &params);
nlohmann::json resolve_merge_conflict(const nlohmann::json &params);

} // namespace singularity::bridge::project_context_bridge
