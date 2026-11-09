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

// The AI work statement of a Matter's month for its client: built from this
// computer's own records of the Matter, its colleagues and its threads, and
// signed with this computer's key, before the folder is asked for.
struct PreparedWorkStatement {
  nlohmann::json signed_file;
};

PreparedWorkStatement prepare_work_statement(const nlohmann::json& params);

// usage.confirmCosts: asks OpenRouter for its own record of each OpenRouter
// call of the Matter in the period not confirmed yet, with the firm's key,
// keeps what it says with the call in the Matter, and asks Legal Machines to
// timestamp this computer's work record if it has grown. Worker only: it
// waits on the network. {confirmed, waiting, stamped}.
nlohmann::json confirm_costs(const nlohmann::json& params);

// Keeps this computer's work record from now on (Usage/UsageStatement's
// work_record), and timestamps its head by Legal Machines while the app runs:
// each time it has grown and been still for ten seconds, checked every
// half-minute, its fingerprint alone sent. Once per process.
void start_work_record();
nlohmann::json finish_work_statement(const PreparedWorkStatement& prepared,
                                     const std::optional<std::string>& folder);

} // namespace singularity::bridge::usage_bridge
