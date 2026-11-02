#include "Bridge/ProviderBridge/ProviderBridge.hpp"

#include "Provider/ProviderConnections/ProviderConnections.hpp"

#include <nlohmann/json.hpp>

namespace singularity::bridge::provider_bridge {
namespace {

using Json = nlohmann::json;

std::string trim(const std::string &value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

Json serialize(const providers::ProviderConnection &connection) {
  Json models = Json::array();
  for (const auto &model : connection.provider.models) {
    models.push_back({{"id", model.id},
                      {"displayName", model.display_name ? Json(*model.display_name) : Json(nullptr)},
                      {"textCapable", model.text_capable},
                      {"excludedReason", model.excluded_reason ? Json(*model.excluded_reason) : Json(nullptr)}});
  }
  return Json{{"id", connection.provider.id},
              {"baseUrl", connection.provider.base_url ? Json(*connection.provider.base_url) : Json(nullptr)},
              {"displayName", connection.provider.display_name ? Json(*connection.provider.display_name) : Json(nullptr)},
              {"models", std::move(models)},
              {"status", connection.state == providers::ProviderConnectionState::Connected
                             ? "connected" : "needs_attention"}};
}

Json serialize_all(const std::vector<providers::ProviderConnection> &connections) {
  Json result = Json::array();
  for (const auto &connection : connections) result.push_back(serialize(connection));
  return result;
}

} // namespace

nlohmann::json list(const nlohmann::json &) {
  return serialize_all(providers::refresh_provider_connections());
}

nlohmann::json configure(const nlohmann::json &params) {
  const auto provider_id = params.at("providerId").get<std::string>();
  const auto secret = params.at("secret").get<std::string>();
  std::optional<std::string> base_url;
  if (params.contains("baseUrl") && params.at("baseUrl").is_string()) {
    const auto value = trim(params.at("baseUrl").get<std::string>());
    if (!value.empty()) base_url = value;
  }
  std::optional<std::string> display_name;
  if (params.contains("displayName") && params.at("displayName").is_string()) {
    const auto value = trim(params.at("displayName").get<std::string>());
    if (!value.empty()) display_name = value;
  }
  return serialize_all(
      providers::configure_provider_connection(provider_id, secret, base_url, display_name));
}

nlohmann::json disconnect(const nlohmann::json &params) {
  return serialize_all(providers::disconnect_provider_connection(
      params.at("providerId").get<std::string>()));
}

nlohmann::json configured_for_onboarding() {
  return serialize_all(providers::configured_provider_connections());
}

} // namespace singularity::bridge::provider_bridge
