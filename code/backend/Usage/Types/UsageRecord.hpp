#pragma once

#include "Provider/ProviderRegistry/ProviderRegistry.hpp"

#include <optional>
#include <string>

namespace singularity::usage {

// Query projection over the authoritative Routing Log. It is deliberately
// not a second persisted store.
struct UsageRecord {
  std::string matter_id;
  std::string operation_id;
  std::string task_id;
  std::optional<std::string> provider_id;
  std::optional<std::string> model_id;
  std::string execution_kind;
  std::string timestamp;
  long long latency_ms = 0;
  providers::UsageMetadata usage;
};

} // namespace singularity::usage
