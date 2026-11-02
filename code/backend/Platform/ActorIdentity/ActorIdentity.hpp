#pragma once

#include <optional>
#include <string>

namespace singularity::platform {

struct ActorIdentity {
  // Attribution identity for a persisted mutation: "seat:<seatId>" once an
  // Organisation seat is authenticated, otherwise "local:<localActorId>".
  // Never derived from device_id - device identity is separate metadata.
  std::string actor_id;
  std::string device_id;
  std::string session_id;
  bool is_authenticated = false;
  // How the signed-in person is shown to people - in the sidebar, beside an
  // approval, as the author of a Word revision. Presentation only: it is a
  // cached copy of what the account says, never a source of authority, which
  // stays with actor_id. Empty for a local operator the account does not name.
  std::optional<std::string> display_name;
  std::optional<std::string> organisation_name;
};

// Resolves attribution for a persisted user mutation. An authenticated seat
// owns actor_id when available. Otherwise actor_id uses a stable local-actor
// identity that is independent of device_id, and is_authenticated is false so
// callers can explicitly distinguish an unauthenticated local operation
// instead of silently treating the device as the user.
ActorIdentity resolve_actor_identity();

// This computer's device id, read once per profile (HOME) and kept: for what
// asks it often or under another lock - an operation's owner, the computer
// that kept a document's version. The first call resolves it under the
// persistence lock.
std::string current_device_id();

} // namespace singularity::platform
