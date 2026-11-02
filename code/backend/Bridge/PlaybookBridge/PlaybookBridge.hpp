#pragma once

#include <nlohmann/json_fwd.hpp>

namespace singularity::bridge::playbook_bridge {

// playbook.* (WorkProduct/Playbook) and reviewTable.prepareChanges
// (WorkProduct/PlaybookChanges): parses, calls the domain, serializes.
nlohmann::json list(const nlohmann::json &params);
nlohmann::json get(const nlohmann::json &params);
nlohmann::json save(const nlohmann::json &params);
nlohmann::json remove(const nlohmann::json &params);
nlohmann::json import_playbook(const nlohmann::json &params);
nlohmann::json export_playbook(const nlohmann::json &params);
nlohmann::json review_table(const nlohmann::json &params);
nlohmann::json prepare_changes(const nlohmann::json &params);
// reviewTable.stopChanges: the lawyer's Stop for the changes being prepared
// from a table (playbook_changes::request_stop).
nlohmann::json stop_changes(const nlohmann::json &params);

} // namespace singularity::bridge::playbook_bridge
