#pragma once

#include <nlohmann/json.hpp>

// The Native API of the time members record on a Matter: recording,
// changing and taking back one's own time, and the Matter's time. Parses,
// calls Billing/, serialises; no rule lives here.
namespace singularity::bridge::time_bridge {

nlohmann::json record(const nlohmann::json &params);
nlohmann::json update(const nlohmann::json &params);
nlohmann::json remove(const nlohmann::json &params);
nlohmann::json list(const nlohmann::json &params);

} // namespace singularity::bridge::time_bridge
