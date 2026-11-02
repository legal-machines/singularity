#pragma once

#include <optional>
#include <string>

namespace singularity::routing {

struct ModelTarget {
  std::string provider_id;
  std::string model_id;
};

// Parses the "providerId:modelId" shape LocalNodeSetup::default_model and
// the frontend's model pickers both use. std::nullopt for an empty/absent
// value or one with no ':' separator.
std::optional<ModelTarget> parse_model_target(const std::optional<std::string> &value);
std::string format_model_target(const ModelTarget &target);

} // namespace singularity::routing
