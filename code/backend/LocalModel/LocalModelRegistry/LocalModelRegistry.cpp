#include "LocalModel/LocalModelRegistry/LocalModelRegistry.hpp"

#include <system_error>

namespace singularity::hardware {

const char *to_string(ModelArchitectureKind kind) {
  switch (kind) {
    case ModelArchitectureKind::Unknown:
      return "unknown";
    case ModelArchitectureKind::EncoderExtraction:
      return "encoder_extraction";
    case ModelArchitectureKind::DenseTransformer:
      return "dense_transformer";
    case ModelArchitectureKind::MixtureOfExperts:
      return "mixture_of_experts";
  }
  // An architecture_kind value outside the enum's real cases is exactly
  // the "unrecognized" case this type exists to represent truthfully -
  // never guessed as one of the known kinds above.
  return "unknown";
}

std::uint64_t directory_size_bytes(const std::filesystem::path &path) {
  std::error_code error;
  if (std::filesystem::is_regular_file(path, error) && !error) {
    return std::filesystem::file_size(path, error);
  }
  error.clear();
  if (!std::filesystem::is_directory(path, error) || error) return 0;

  std::uint64_t total = 0;
  for (const auto &entry : std::filesystem::directory_iterator(path, error)) {
    if (error) break;
    std::error_code file_error;
    if (entry.is_regular_file(file_error) && !file_error) {
      total += entry.file_size(file_error);
    }
  }
  return total;
}

LocalModelRegistry::LocalModelRegistry(LocalModelRegistry &&other) noexcept {
  const std::lock_guard<std::mutex> lock(other.mutex_);
  entries_ = std::move(other.entries_);
}

std::vector<LocalModelRegistry::Entry> LocalModelRegistry::snapshot() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return entries_;
}

LocalModelRegistry::HardwareAwareLiveStateProvider LocalModelRegistry::hardware_aware(LiveStateProvider provider) {
  if (!provider) return nullptr;
  return [provider = std::move(provider)](const HardwareCapabilities &) { return provider(); };
}

void LocalModelRegistry::register_model(LocalModelDescriptor descriptor,
                                        LiveStateProvider live_state_provider,
                                        UnloadCallback unload_callback) {
  register_model(std::move(descriptor), hardware_aware(std::move(live_state_provider)), std::move(unload_callback));
}

void LocalModelRegistry::register_model(LocalModelDescriptor descriptor, std::nullptr_t,
                                        UnloadCallback unload_callback) {
  register_model(std::move(descriptor), HardwareAwareLiveStateProvider{}, std::move(unload_callback));
}

void LocalModelRegistry::register_model(LocalModelDescriptor descriptor,
                                        HardwareAwareLiveStateProvider live_state_provider,
                                        UnloadCallback unload_callback) {
  const std::lock_guard<std::mutex> lock(mutex_);
  entries_.push_back(Entry{std::move(descriptor), std::move(live_state_provider), std::move(unload_callback)});
}

void LocalModelRegistry::upsert_model(LocalModelDescriptor descriptor,
                                      LiveStateProvider live_state_provider,
                                      UnloadCallback unload_callback) {
  upsert_model(std::move(descriptor), hardware_aware(std::move(live_state_provider)), std::move(unload_callback));
}

void LocalModelRegistry::upsert_model(LocalModelDescriptor descriptor, std::nullptr_t,
                                      UnloadCallback unload_callback) {
  upsert_model(std::move(descriptor), HardwareAwareLiveStateProvider{}, std::move(unload_callback));
}

void LocalModelRegistry::upsert_model(LocalModelDescriptor descriptor,
                                      HardwareAwareLiveStateProvider live_state_provider,
                                      UnloadCallback unload_callback) {
	const std::lock_guard<std::mutex> lock(mutex_);
	for (auto &entry : entries_) {
		if (entry.descriptor.id != descriptor.id) continue;
		entry = Entry{std::move(descriptor), std::move(live_state_provider),
		              std::move(unload_callback)};
		return;
	}
	entries_.push_back(Entry{std::move(descriptor), std::move(live_state_provider),
	                         std::move(unload_callback)});
}

void LocalModelRegistry::unregister_model(const std::string& id) {
	const std::lock_guard<std::mutex> lock(mutex_);
	for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
		if (iterator->descriptor.id != id) continue;
		entries_.erase(iterator);
		return;
	}
}

std::vector<LocalModelDescriptor> LocalModelRegistry::descriptors() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LocalModelDescriptor> result;
  result.reserve(entries_.size());
  for (const auto &entry : entries_) result.push_back(entry.descriptor);
  return result;
}

LocalModelStatus LocalModelRegistry::status_for_entry(const Entry &entry, const HardwareCapabilities &hardware) {
  std::error_code error;
  const bool pathPresent = std::filesystem::exists(entry.descriptor.install_path, error) && !error;
  const LocalModelLiveState live =
      entry.live_state_provider ? entry.live_state_provider(hardware) : LocalModelLiveState{};
  // "Installed" means this computer can actually run this model, and for a
  // model local inference found in its folder without a package of its own
  // that is the runtime's own answer, not a filesystem probe: such a model
  // carries an install_path that names no file (so the Library never sizes or
  // removes what Singularity did not install), and probing it would declare a
  // model the person can see and load "not installed on this computer". A
  // managed package still answers by its real path.
  const bool runtimeHasIt = live.load_state != LocalModelLoadState::NotInstalled;
  const bool installed = pathPresent || runtimeHasIt;

  LocalModelStatus status;
  status.descriptor = entry.descriptor;
  status.installed = installed;
  status.disk_bytes = pathPresent ? directory_size_bytes(entry.descriptor.install_path) : 0;
  status.live = live;
  status.compatibility = assess_compatibility(entry.descriptor.preferred_backends,
                                              entry.descriptor.resource_requirement, hardware);
  status.load_impact = assess_load_impact(entry.descriptor.resource_requirement, hardware);
  return status;
}

std::vector<LocalModelStatus> LocalModelRegistry::list_status() const {
  const std::vector<Entry> entries = snapshot();
  std::vector<LocalModelStatus> result;
  result.reserve(entries.size());
  // Detected once per call, not per entry - the machine does not change
  // between two registered models in the same listing.
  const auto hardware = detect_hardware_capabilities();
  for (const auto &entry : entries) result.push_back(status_for_entry(entry, hardware));
  return result;
}

std::optional<LocalModelStatus> LocalModelRegistry::status_for(const std::string &model_id) const {
  for (const auto &entry : snapshot())
    if (entry.descriptor.id == model_id) return status_for_entry(entry, detect_hardware_capabilities());
  return std::nullopt;
}

ModelContextCapability resolve_model_context_capability(const LocalModelDescriptor &descriptor,
                                                        const LocalModelLiveState &live) {
  ModelContextCapability capability;
  capability.native_context_tokens = descriptor.execution_profile.native_context_length;
  capability.planned_context_tokens = live.planned_context_tokens;
  // Granted is a fact about the CURRENT load only: a plan retained from an
  // earlier load says nothing about what the next one will be granted.
  if (live.load_state == LocalModelLoadState::Ready) capability.granted_context_tokens = live.effective_context_tokens;
  return capability;
}

std::vector<std::string> LocalModelRegistry::release_reclaimable_memory() const {
  std::vector<std::string> released;
  const auto hardware = detect_hardware_capabilities();
  for (const auto &entry : snapshot()) {
    if (entry.descriptor.required || !entry.unload_callback) continue;
    const auto live = entry.live_state_provider ? entry.live_state_provider(hardware) : LocalModelLiveState{};
    if (live.load_state != LocalModelLoadState::Ready) continue;
    // A module still answering is skipped, never stopped under the person
    // waiting for it, and never counted as released.
    if (!entry.unload_callback()) continue;
    released.push_back(entry.descriptor.id);
  }
  return released;
}

} // namespace singularity::hardware
