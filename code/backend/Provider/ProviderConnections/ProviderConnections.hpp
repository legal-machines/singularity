#pragma once

#include "Platform/AppSettings/AppSettings.hpp"

#include <optional>
#include <string>
#include <vector>

namespace singularity::providers {

enum class ProviderConnectionState { Connected, NeedsAttention };

struct ProviderConnection {
  platform::ConnectedProvider provider;
  ProviderConnectionState state = ProviderConnectionState::NeedsAttention;
};

// Fast local projection used while composing onboarding status; no network.
std::vector<ProviderConnection> configured_provider_connections();

// Revalidates credentials and refreshes provider-owned model discovery.
std::vector<ProviderConnection> refresh_provider_connections();

// `display_name` is required (and stored) for a real custom connection
// (Provider/ProviderRegistry.hpp's own is_valid_custom_provider_id()) - the
// universal "connect any provider" mechanism's own human label, distinct
// from `provider_id`'s internal slug. Ignored for the fixed built-in
// presets ("openai"/"anthropic"), whose display name is already a stable,
// known constant.
std::vector<ProviderConnection> configure_provider_connection(
    const std::string &provider_id, const std::string &secret,
    const std::optional<std::string> &base_url,
    const std::optional<std::string> &display_name = std::nullopt);

std::vector<ProviderConnection> disconnect_provider_connection(
    const std::string &provider_id);

} // namespace singularity::providers
