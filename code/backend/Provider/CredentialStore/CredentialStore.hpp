#pragma once

#include <optional>
#include <stdexcept>
#include <string>

namespace singularity::platform {

// A stored credential that exists but cannot be read right now - on macOS,
// while the Mac is locked, which is its protection working. Nothing is wrong
// with it: whoever needs it may wait and ask again.
class CredentialUnavailable : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Native, provider-only credential storage. Implementations must use OS data
// protection and must never fall back to settings.json or renderer storage.
std::optional<std::string> get_provider_credential(const std::string &provider_id);
void set_provider_credential(const std::string &provider_id, const std::string &credential);
void delete_provider_credential(const std::string &provider_id);

// Singularity's own secrets kept here beside the providers' keys: this
// computer's coordination key and each Matter's keys (MatterCoordination
// kComputerKeySecret, matterKeySecret) and the model catalog's session
// (ModelPackageManager kSessionCredential). No provider is ever named as one,
// so connecting, disconnecting or receiving a provider never writes over or
// removes one.
inline bool is_reserved_credential_name(const std::string &name) {
  return name.rfind("lm-coordination-", 0) == 0 || name.rfind("lm-matter-key-", 0) == 0 || name == "singularity-session";
}

// What a person reads when a stored credential exists but cannot be read right
// now - what happened and what to do - in this store's own terms (macOS keeps
// the file unreadable while the Mac is locked).
std::string credential_unreadable_explanation(const std::string &provider_name);

} // namespace singularity::platform
