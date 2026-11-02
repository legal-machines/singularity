#include "Platform/ActorIdentity/ActorIdentity.hpp"

#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"

#include <cstdlib>
#include <mutex>

namespace singularity::platform {
namespace {

// A device, actor or session identity: 24 digits that are 96 bits from the
// system's secure source (Platform/SecureRandom), so no two computers or
// people share one by chance.
std::string generateIdentity(const std::string &prefix) {
  return prefix + secure_random_hex(12);
}

const std::string &currentSessionId() {
  static const std::string sessionId = generateIdentity("session_");
  return sessionId;
}

} // namespace

ActorIdentity resolve_actor_identity() {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  AppSettings settings = load_app_settings();
  bool settingsChanged = false;
  if (settings.device_id.empty() == true) {
    settings.device_id = generateIdentity("device_");
    settingsChanged = true;
  }
  // local_actor_id is deliberately generated independently of device_id: it
  // names the local operator, not the machine, so the two identities never
  // collapse into one value that would fossilise "this device" as "the user".
  if (settings.local_actor_id.empty() == true) {
    settings.local_actor_id = generateIdentity("actor_");
    settingsChanged = true;
  }
  if (settingsChanged == true) save_app_settings(settings);

  ActorIdentity identity;
  identity.device_id = settings.device_id;
  identity.session_id = currentSessionId();
  if (settings.account.has_value() == true && settings.account->seat_id.empty() == false) {
    identity.actor_id = "seat:" + settings.account->seat_id;
    identity.is_authenticated = true;
    identity.display_name = settings.account->display_name;
    identity.organisation_name = settings.account->organisation_name;
    return identity;
  }

  identity.actor_id = "local:" + settings.local_actor_id;
  identity.is_authenticated = false;
  // The person Settings names on this computer, for presentation only.
  identity.display_name = settings.local_display_name;
  identity.organisation_name = settings.local_organisation_name;
  return identity;
}

std::string current_device_id() {
  // Never destroyed: asked from worker threads that may outlive static
  // destruction at exit.
  struct Cache {
    std::mutex mutex;
    std::string home;
    std::string device;
  };
  static Cache *cache = new Cache();
  const char *current = std::getenv("HOME");
  const std::string currentHome = current != nullptr ? current : "";
  {
    const std::lock_guard<std::mutex> lock(cache->mutex);
    if (!cache->device.empty() && cache->home == currentHome) return cache->device;
  }
  // Resolved outside this lock: resolving takes the persistence lock.
  const std::string resolved = resolve_actor_identity().device_id;
  const std::lock_guard<std::mutex> lock(cache->mutex);
  cache->device = resolved;
  cache->home = currentHome;
  return resolved;
}

} // namespace singularity::platform
