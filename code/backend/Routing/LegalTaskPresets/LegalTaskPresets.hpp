#pragma once

#include "Professional/StructuredResults/StructuredResults.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace singularity::context {

struct LegalTaskPreset {
  std::string_view id;
  int version;
  std::string_view name;
  std::string_view instructions;
  std::string_view expected_output_style;
	// The typed professional results this legal task's own output contract
	// includes (see StructuredResults.hpp). Declared here because the preset IS
	// the resolved objective semantics the user chose - not something Singularity
	// may infer from request wording later.
	professional::StructuredResultKinds structured_results = professional::StructuredResultKind::None;
};

// The provider-neutral, versioned registry used by task configuration and
// prompt assembly. IDs are persistence/API values and therefore stable.
const std::vector<LegalTaskPreset> &legal_task_presets();

// Returns no value for an unknown ID or a version that is no longer present.
// Callers must not silently substitute General for persisted unknown values.
std::optional<LegalTaskPreset> find_legal_task_preset(
    std::string_view id, int version);

inline constexpr std::string_view kDefaultLegalTaskPresetId = "general";
inline constexpr int kDefaultLegalTaskPresetVersion = 1;

// The one current (highest-version) preset per id, in registry order,
// excluding General - the exact list Chat's composer offers through
// progressive disclosure. General is never chosen explicitly; an unset
// legal task always resolves to it instead
// (kDefaultLegalTaskPresetId/Version above). Backend truth only - a
// frontend caller must read this instead of hardcoding its own
// id/version/name list.
const std::vector<LegalTaskPreset> &selectable_legal_task_presets();

} // namespace singularity::context
