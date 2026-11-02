#pragma once

#include "WorkProduct/WorkProductExport/WorkProductExport.hpp"

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>

namespace singularity::bridge::work_product_bridge {

nlohmann::json list_types(const nlohmann::json& params);
nlohmann::json create_type(const nlohmann::json& params);
nlohmann::json list_objects(const nlohmann::json& params);
nlohmann::json get_detail(const nlohmann::json& params);
nlohmann::json create(const nlohmann::json& params);
nlohmann::json retain_task_result(const nlohmann::json& params);
nlohmann::json update(const nlohmann::json& params);
nlohmann::json update_issue_status(const nlohmann::json& params);
nlohmann::json restore_revision(const nlohmann::json& params);
nlohmann::json remove(const nlohmann::json& params);

work_product::PreparedExport prepare_export(const nlohmann::json &params);
nlohmann::json finish_export(const work_product::PreparedExport &prepared,
                             const std::optional<std::string> &selected_path);

} // namespace singularity::bridge::work_product_bridge
