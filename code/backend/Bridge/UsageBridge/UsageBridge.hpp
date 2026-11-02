#pragma once

#include "Usage/UsageStatement/UsageStatement.hpp"

#include <nlohmann/json.hpp>

#include <optional>

namespace singularity::bridge::usage_bridge {

nlohmann::json list(const nlohmann::json& params);
nlohmann::json get_summary(const nlohmann::json& params);

struct PreparedStatement {
  nlohmann::json statement;
  usage::StructuredStatementFormat structured_format = usage::StructuredStatementFormat::Json;
};

PreparedStatement prepare_statement(const nlohmann::json& params);
nlohmann::json finish_statement(
    const PreparedStatement& prepared,
    const std::optional<std::string>& destination_directory);

} // namespace singularity::bridge::usage_bridge
