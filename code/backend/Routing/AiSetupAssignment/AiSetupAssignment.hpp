#pragma once

#include "Routing/AiSetup/AiSetup.hpp"

namespace singularity::routing {

// Looks up the real, currently eligible candidate for `role` - an installed
// local model or a connected provider's own reported model - and applies
// AiSetup.hpp's is_local_model_compatible()/is_provider_model_compatible()
// to it. Throws a plain-language std::runtime_error when the pairing is
// invalid or the model does not currently exist. The one validation path
// every AI Setup mutation (Node/Project/Chat) shares, so an incompatible
// assignment is rejected identically everywhere it can be attempted -
// separated from Routing/AiSetup/AiSetup.hpp because this one touches the
// live local-model/provider registries, not just pure role/family logic.
void validate_role_model_assignment(AiSetupRole role, const ModelTarget &target);

} // namespace singularity::routing
