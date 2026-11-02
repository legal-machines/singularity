#include "Routing/Types/ModelTarget.hpp"

namespace singularity::routing {

std::optional<ModelTarget> parse_model_target(const std::optional<std::string> &value) {
  if (!value || value->empty()) return std::nullopt;
  const auto separator = value->find(':');
  if (separator == std::string::npos || separator == 0 || separator == value->size() - 1) {
    return std::nullopt;
  }
  return ModelTarget{value->substr(0, separator), value->substr(separator + 1)};
}

std::string format_model_target(const ModelTarget &target) {
  return target.provider_id + ":" + target.model_id;
}

} // namespace singularity::routing
