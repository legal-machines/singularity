#pragma once

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <vector>

namespace singularity::bridge::consilium_bridge {

nlohmann::json list(const nlohmann::json& params);
nlohmann::json create(const nlohmann::json& params);
nlohmann::json update(const nlohmann::json& params);
nlohmann::json duplicate(const nlohmann::json& params);
nlohmann::json import_path(const nlohmann::json& params, const std::string& path);
nlohmann::json import_selected(const nlohmann::json& params, const std::vector<std::string>& paths);
std::string export_default_name(const nlohmann::json& params);
nlohmann::json export_path(const nlohmann::json& params, const std::string& path);
nlohmann::json export_selected(const nlohmann::json& params, const std::optional<std::string>& path);

} // namespace singularity::bridge::consilium_bridge
