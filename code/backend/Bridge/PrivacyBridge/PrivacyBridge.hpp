#pragma once

#include <nlohmann/json_fwd.hpp>

namespace singularity::bridge::privacy_bridge {

nlohmann::json get_overview(const nlohmann::json &params);
// privacy.getCoverage: the overview's counts and Guard's state, without what
// left this device - nothing of the routing log is read.
nlohmann::json get_coverage(const nlohmann::json &params);
nlohmann::json list_attention_documents(const nlohmann::json &params);
nlohmann::json list_entities(const nlohmann::json &params);
nlohmann::json get_entity_detail(const nlohmann::json &params);
nlohmann::json resolve_ambiguity(const nlohmann::json &params);
nlohmann::json split_entity_alias(const nlohmann::json &params);
nlohmann::json add_entity_alias(const nlohmann::json &params);
nlohmann::json remove_entity_alias(const nlohmann::json &params);
// What one operation's protected requests sent, passage by passage, beside the
// Project's own text: what was hidden, and what replaced it.
nlohmann::json get_passage_disclosure(const nlohmann::json &params);
nlohmann::json get_operation_disclosure(const nlohmann::json &params);

} // namespace singularity::bridge::privacy_bridge
