#include "Bridge/ChatPreviewBridge/ChatPreviewBridge.hpp"

#include "Bridge/ChatBridge/ChatBridge.hpp"
#include "Chat/ChatOutboundPreview/ChatOutboundPreview.hpp"

#include <nlohmann/json.hpp>

namespace singularity::bridge::chat_preview_bridge {

nlohmann::json resolve_protection_state(const nlohmann::json &params) {
  // The same validated parse both Phase 22 and Phase 23 use - never a
  // second reading of a one-time override or a source-policy mode.
  const chat::ChatProtectionState state =
      chat::resolve_protection_state(chat_bridge::chat_request_setup(params));
  return nlohmann::json{
      {"destination", state.destination},
      {"primaryResultRole", state.primary_result_role},
      {"primaryModel", state.primary_model
           ? nlohmann::json{{"providerId", state.primary_model->provider_id},
                            {"modelId", state.primary_model->model_id}}
           : nlohmann::json(nullptr)},
      {"protectionMode", state.protection_mode},
      {"protectionReady", state.is_protection_ready},
      {"protectionLoading", state.is_protection_loading},
      {"protectionMessage", state.protection_message.empty() ? nlohmann::json(nullptr)
                                                             : nlohmann::json(state.protection_message)},
      {"keptHere", state.kept_here ? nlohmann::json(*state.kept_here) : nlohmann::json(nullptr)},
      {"keptHereMessage", state.kept_here ? nlohmann::json(state.kept_here_message) : nlohmann::json(nullptr)},
  };
}

nlohmann::json preview_outbound_request(const nlohmann::json &params) {
  return chat::preview_chat_outbound_request(chat_bridge::chat_request_composition(params));
}

} // namespace singularity::bridge::chat_preview_bridge
