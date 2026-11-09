#include "Routing/AiSetupAssignment/AiSetupAssignment.hpp"

#include "Platform/AppSettings/AppSettings.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace singularity::routing {

void validate_role_model_assignment(AiSetupRole role, const ModelTarget &target) {
  // Accepting an assignment the runtime then ignores would leave a person
  // believing they had changed what protects their data.
  if (!is_role_assignable(role)) {
    throw std::runtime_error(fixed_role_reason(role));
  }
  if (target.provider_id == "local") {
    const auto statuses = production_local_model_registry().list_status();
    const auto found = std::find_if(statuses.begin(), statuses.end(),
        [&](const hardware::LocalModelStatus &status) { return status.descriptor.id == target.model_id; });
    if (found == statuses.end() || !found->installed) {
      throw std::runtime_error("This model is not installed on this computer.");
    }
    if (!is_local_model_compatible(role, found->descriptor.family,
                                   found->descriptor.execution_profile.architecture_kind)) {
      throw std::runtime_error(std::string(display_label(role)) + " does not accept this model.");
    }
    return;
  }

  const auto settings = platform::load_app_settings();
  const auto &connected_providers = settings.local_node_setup.connected_providers;
  const auto provider = std::find_if(connected_providers.begin(), connected_providers.end(),
      [&](const platform::ConnectedProvider &entry) { return entry.id == target.provider_id; });
  if (provider == connected_providers.end()) {
    throw std::runtime_error("This provider is not connected.");
  }
  const auto model = std::find_if(provider->models.begin(), provider->models.end(),
      [&](const providers::ModelDescriptor &entry) { return entry.id == target.model_id; });
  if (model == provider->models.end()) {
    throw std::runtime_error("This model is not offered by the connected provider.");
  }
  if (!is_provider_model_compatible(role, model->text_capable)) {
    throw std::runtime_error(std::string(display_label(role)) + " does not accept this model.");
  }
}

} // namespace singularity::routing
