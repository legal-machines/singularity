#pragma once

#include <nlohmann/json_fwd.hpp>

namespace singularity::bridge::context_bridge {

// `execute_model` was removed for Beta 1: it made a real billed provider call
// and recorded nothing - no operation, no usage, no provenance. Use the
// canonical chat request path instead.
nlohmann::json run_task(const nlohmann::json &params);
nlohmann::json preview_outbound_request(const nlohmann::json &params);
nlohmann::json reveal_protected_occurrence(const nlohmann::json &params);
nlohmann::json close_outbound_preview(const nlohmann::json &params);
nlohmann::json retry_task(const nlohmann::json &params);
nlohmann::json revise_task(const nlohmann::json &params);
nlohmann::json run_consilium(const nlohmann::json &params);
nlohmann::json synthesize_consilium(const nlohmann::json &params);
nlohmann::json list_workflows(const nlohmann::json &params);
nlohmann::json run_workflow(const nlohmann::json &params);

} // namespace singularity::bridge::context_bridge
