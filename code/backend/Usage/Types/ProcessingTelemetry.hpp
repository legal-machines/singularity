#pragma once

#include <string>

namespace singularity::usage {

// Safe operational counters attached to one request. This value type does
// not contain prompts, responses, credentials, or Papinian reverse mappings.
struct ProcessingTelemetry {
  int version = 1;
  std::string rule = "registered-operation-units-v1";
  int local_operation_units = 0;
  int external_operation_units = 0;
};

} // namespace singularity::usage
