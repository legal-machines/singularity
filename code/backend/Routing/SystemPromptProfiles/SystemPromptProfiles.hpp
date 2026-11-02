#pragma once

#include <optional>
#include <string_view>
#include <vector>

namespace singularity::context {

struct SystemPromptProfile {
  std::string_view id;
  int version;
  std::string_view instructions;
};

// The provider-neutral, versioned registry used by prompt assembly.
// Persisted Tasks keep the ID and version so a later profile revision cannot
// silently rewrite history - see find_system_prompt_profile()'s own doc
// comment.
const std::vector<SystemPromptProfile> &system_prompt_profiles();

// Returns no value for an unknown ID or a version that is no longer
// present. Callers must not silently substitute a different version for a
// persisted, unknown one (mirrors context::find_legal_task_preset()).
std::optional<SystemPromptProfile> find_system_prompt_profile(
    std::string_view id, int version);

inline constexpr std::string_view kDefaultSystemPromptProfileId =
    "legal-machines";
// v4 keeps v3's general framing and adds document fidelity: source-bound work
// follows the supplied material's own terminology, structure and register
// unless the user says otherwise. v1-v3 remain resolvable byte-for-byte for
// persisted history and are never edited in place.
//
// This profile is REQUIRED content on every request, so v4's ~240 extra
// tokens are a real, permanent capacity cost for small-context models - an
// explicit owner product decision taken for answer quality. The moved
// boundary is pinned on both sides by ChatExecution.test.cpp's own 4096-token
// local cases rather than left to be discovered in production.
inline constexpr int kDefaultSystemPromptProfileVersion = 4;

} // namespace singularity::context
