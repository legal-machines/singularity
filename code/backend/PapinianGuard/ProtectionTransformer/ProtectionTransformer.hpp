#pragma once

#include "PapinianGuard/Policy/Policy.hpp"

#include <string>
#include <unordered_map>

namespace singularity::protected_api {

struct TransformResult {
  std::string transformed_text;
  // placeholder -> original value. Request-scoped only: never persisted,
  // never logged - see Routing/InferenceExecutor/InferenceExecutor.cpp, the only caller that
  // holds one of these, and only for the lifetime of a single request.
  std::unordered_map<std::string, std::string> mapping;
};

// Replaces every span `detect()` finds in `text` with a stable placeholder
// (e.g. "AMOUNT_1", "EMAIL_2", "TERM_1"). The same original value always
// gets the same placeholder within one call, so a repeated email address or
// custom term collapses to one placeholder rather than a new one each time.
TransformResult transform(const std::string &text, const Policy &policy);

// Replaces every placeholder in `text` that appears as a key in `mapping`
// with its original value. Placeholders the model altered or dropped are
// left as-is - there is nothing to restore them from.
std::string restore(const std::string &text, const std::unordered_map<std::string, std::string> &mapping);

} // namespace singularity::protected_api
