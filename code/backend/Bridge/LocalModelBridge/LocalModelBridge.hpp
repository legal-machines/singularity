#pragma once

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>

namespace singularity::bridge::local_model_bridge {

nlohmann::json list(const nlohmann::json &params);
nlohmann::json runtime_state(const nlohmann::json &params);
nlohmann::json papinian_status(const nlohmann::json &params);
nlohmann::json authorize(const nlohmann::json &params);
nlohmann::json sync(const nlohmann::json &params);
nlohmann::json load(const nlohmann::json &params);
nlohmann::json unload(const nlohmann::json &params);
nlohmann::json remove(const nlohmann::json& params);

nlohmann::json use_existing_file(const std::optional<std::string> &selected_path);
nlohmann::json forget_existing_file(const nlohmann::json &params);

} // namespace singularity::bridge::local_model_bridge
