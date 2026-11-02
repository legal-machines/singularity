#pragma once

#include <nlohmann/json_fwd.hpp>

namespace singularity::bridge::review_table_bridge {

// reviewTable.* (WorkProduct/ReviewTable): parses, calls the domain, serializes.
nlohmann::json list(const nlohmann::json &params);
nlohmann::json get(const nlohmann::json &params);
nlohmann::json create(const nlohmann::json &params);
nlohmann::json update(const nlohmann::json &params);
nlohmann::json remove(const nlohmann::json &params);
nlohmann::json run(const nlohmann::json &params);
nlohmann::json stop(const nlohmann::json &params);
nlohmann::json export_table(const nlohmann::json &params);

} // namespace singularity::bridge::review_table_bridge
