#pragma once

#include "Agent/AiAppChannel/AiAppChannel.hpp"

namespace singularity::agents {

// The program that started this helper - the AI app - as the system knows
// it: its path, and its code signature when the system verifies one: on macOS
// signed by a certificate Apple issued (Developer ID or Apple's own). Linux
// verifies none. A program it cannot verify is named by its path alone and
// called unverified.
//
// This labels who asked and holds a key to the program first seen using it;
// it is not a defence against a program running as the same user, which
// could read the key wherever the AI app keeps it.
AiAppCaller identify_caller();

} // namespace singularity::agents
