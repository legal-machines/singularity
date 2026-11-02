#pragma once

#include <nlohmann/json_fwd.hpp>

namespace singularity::bridge::provider_bridge {

nlohmann::json list(const nlohmann::json &params);
nlohmann::json configure(const nlohmann::json &params);
nlohmann::json disconnect(const nlohmann::json &params);
nlohmann::json configured_for_onboarding();

} // namespace singularity::bridge::provider_bridge
