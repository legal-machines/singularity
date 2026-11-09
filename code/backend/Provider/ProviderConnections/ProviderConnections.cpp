#include "Provider/ProviderConnections/ProviderConnections.hpp"

#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Provider/AccountGate/AccountGate.hpp"
#include "Platform/AppPaths/AppPaths.hpp"
#include "Provider/CredentialStore/CredentialStore.hpp"
#include "Provider/ProviderRegistry/ProviderRegistry.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <stdexcept>

namespace singularity::providers {
namespace {

ProviderConnection local_status(const platform::ConnectedProvider &provider) {
  try {
    const auto credential = platform::get_provider_credential(provider.id);
    return {provider, credential && !credential->empty()
                          ? ProviderConnectionState::Connected
                          : ProviderConnectionState::NeedsAttention};
  } catch (...) {
    return {provider, ProviderConnectionState::NeedsAttention};
  }
}

} // namespace

std::vector<ProviderConnection> configured_provider_connections() {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto settings = platform::load_app_settings();
  std::vector<ProviderConnection> result;
  result.reserve(settings.local_node_setup.connected_providers.size());
  for (const auto &provider : settings.local_node_setup.connected_providers) result.push_back(local_status(provider));
  return result;
}

std::vector<ProviderConnection> refresh_provider_connections() {
  std::vector<platform::ConnectedProvider> connected;
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    std::error_code gateway_session_error;
    std::filesystem::remove(platform::app_settings_directory() / "gateway.session", gateway_session_error);
    connected = platform::load_app_settings().local_node_setup.connected_providers;
  }
  // Each account is asked over the network with no lock held: one provider
  // slow to answer, or a connection that dropped, waits out its own timeout
  // alone, never with every save and read of the app behind it.
  std::vector<ProviderConnection> result;
  std::map<std::string, std::vector<ModelDescriptor>> fresh;
  for (auto &provider : connected) {
    const auto credential = platform::get_provider_credential(provider.id);
    if (!credential || credential->empty()) {
      result.push_back({provider, ProviderConnectionState::NeedsAttention});
      continue;
    }
    try {
      const auto check = test_connection(provider.id, *credential, provider.base_url);
      if (!check.ok) {
        result.push_back({provider, ProviderConnectionState::NeedsAttention});
        continue;
      }
      provider.models = check.models;
      fresh[provider.id] = check.models;
      result.push_back({provider, ProviderConnectionState::Connected});
    } catch (...) {
      result.push_back({provider, ProviderConnectionState::NeedsAttention});
    }
  }
  if (fresh.empty() == false) {
    // The models each answered with, kept under the lock into the settings as
    // they stand now: an account connected or removed meanwhile stays so.
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    auto settings = platform::load_app_settings();
    bool isChanged = false;
    for (auto &provider : settings.local_node_setup.connected_providers) {
      const auto found = fresh.find(provider.id);
      if (found == fresh.end()) continue;
      provider.models = found->second;
      isChanged = true;
    }
    if (isChanged) platform::save_app_settings(settings);
  }
  return result;
}

std::vector<ProviderConnection> configure_provider_connection(
    const std::string &provider_id, const std::string &secret,
    const std::optional<std::string> &base_url,
    const std::optional<std::string> &display_name) {
  require_may_connect_accounts();
  const bool is_custom = !is_known_provider(provider_id) && is_valid_custom_provider_id(provider_id);
  if (!is_known_provider(provider_id) && !is_custom) throw std::runtime_error("Unknown model provider.");
  if (secret.empty()) throw std::runtime_error("Enter an API credential.");
  if (is_custom && (!display_name || display_name->empty())) {
    throw std::runtime_error("Enter a name for this provider.");
  }
  if (provider_requires_base_url(provider_id) &&
      (!base_url || !is_valid_base_url(*base_url))) {
    throw std::runtime_error(
        "Enter a valid HTTPS endpoint URL, or an http:// localhost URL for a local server.");
  }
  const auto effective_base_url = provider_requires_base_url(provider_id) ? base_url : std::nullopt;
  const auto check = test_connection(provider_id, secret, effective_base_url);
  if (!check.ok) throw std::runtime_error(check.error);

  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    auto settings = platform::load_app_settings();
    auto &configured = settings.local_node_setup.connected_providers;
    const auto previous_secret = platform::get_provider_credential(provider_id);
    platform::set_provider_credential(provider_id, secret);
    try {
      const auto existing = std::find_if(configured.begin(), configured.end(),
          [&](const auto &item) { return item.id == provider_id; });
      platform::ConnectedProvider provider{
          provider_id, effective_base_url, check.models, is_custom ? display_name : std::nullopt};
      if (existing == configured.end()) configured.push_back(std::move(provider));
      else *existing = std::move(provider);
      platform::save_app_settings(settings);
    } catch (...) {
      if (previous_secret) platform::set_provider_credential(provider_id, *previous_secret);
      else platform::delete_provider_credential(provider_id);
      throw;
    }
  }
  return refresh_provider_connections();
}

std::vector<ProviderConnection> disconnect_provider_connection(const std::string &provider_id) {
  require_may_connect_accounts();
  if (!is_known_provider(provider_id) && !is_valid_custom_provider_id(provider_id)) {
    throw std::runtime_error("Unknown model provider.");
  }
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    auto settings = platform::load_app_settings();
    platform::delete_provider_credential(provider_id);
    auto &configured = settings.local_node_setup.connected_providers;
    configured.erase(std::remove_if(configured.begin(), configured.end(),
        [&](const auto &item) { return item.id == provider_id; }), configured.end());
    // role_assignments is the single canonical Node default AI Setup store
    // for every Beta 1 role, including Reasoning - a disconnected
    // provider's own models cannot remain assigned to any role. Update it
    // first; default_model below is only ever a derived compatibility
    // mirror of its own Reasoning entry, never an independent thing to
    // clear separately.
    auto &role_assignments = settings.local_node_setup.role_assignments;
    role_assignments.erase(
        std::remove_if(role_assignments.begin(), role_assignments.end(),
                       [&](const auto &assignment) { return assignment.provider_id == provider_id; }),
        role_assignments.end());
    const auto reasoning = std::find_if(role_assignments.begin(), role_assignments.end(),
        [](const auto &assignment) { return assignment.role == "reasoning"; });
    settings.local_node_setup.default_model = reasoning == role_assignments.end()
        ? std::optional<std::string>()
        : std::optional<std::string>(reasoning->provider_id + ":" + reasoning->model_id);
    platform::save_app_settings(settings);
  }
  return refresh_provider_connections();
}

} // namespace singularity::providers
