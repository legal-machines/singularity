#pragma once

#include <nlohmann/json_fwd.hpp>

// Phase 23's two Chat trust entry points. They live beside - not inside -
// Bridge/ChatBridge because they are the only Chat wire methods that reach
// the canonical request-preparation path (Routing/MatterRequestExecution
// and, through it, the provider/local-model runtime). Keeping them here
// leaves Chat's ordinary list/create/rename/source/setup bridge free of
// that build footprint, exactly as Bridge/ContextBridge already isolates
// Phase 11's matter.previewOutboundRequest for the same reason.
namespace singularity::bridge::chat_preview_bridge {

// chat.resolveProtectionState - the calm composer trust state (Chat/
// ChatOutboundPreview/ChatOutboundPreview.hpp). Prepares nothing, runs no
// retrieval and never reads the prompt, so no keystroke can trigger request
// preparation.
nlohmann::json resolve_protection_state(const nlohmann::json &params);

// chat.previewOutboundRequest - "What leaves this device?" for one pending
// Chat request. Resolves Phase 21 sources and Phase 22 operation setup,
// prepares the request through the canonical MatterRequestExecution path
// and returns the existing Phase 11 Papinian preview. Makes no provider/
// network call, creates no MatterTask and persists nothing.
nlohmann::json preview_outbound_request(const nlohmann::json &params);

} // namespace singularity::bridge::chat_preview_bridge
