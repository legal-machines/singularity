#pragma once

#include <nlohmann/json_fwd.hpp>

namespace singularity::bridge::routing_bridge {

// Role/capability metadata for every Beta 1 AI Setup role - the eligible
// local/provider models a role card's picker or drop target may offer. See
// Routing/AiSetup/AiSetup.hpp's compatible_local_families()/
// is_local_model_compatible()/is_provider_model_compatible().
nlohmann::json get_ai_setup_capabilities(const nlohmann::json &params);

// The one resolver every AI Setup surface reads from - Node default ->
// Project override -> Chat override, per Routing/AiSetup/AiSetup.hpp's
// resolve_effective_setup(). Params: { projectId?: string, chatId?: string }.
// A chatId without a projectId is an error - a Chat cannot resolve without
// knowing which Project's overrides sit above it.
nlohmann::json get_effective_ai_setup(const nlohmann::json &params);

} // namespace singularity::bridge::routing_bridge
