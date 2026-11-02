#pragma once

namespace singularity::platform {

// Pins this process to the accessory (UIElement) activation policy, so a
// command-line or test binary never takes a Dock tile. Applied automatically
// before main() for every non-bundle executable (see ConsoleActivationPolicy.mm
// and singularity_add_executable in CMakeLists.txt).
//
// Call it again from main() in a binary that still measures as
// type="Foreground" under `lsappinfo`: a dependency initialised after the
// pre-main constructor can promote the process back, and re-asserting once the
// process is fully up is the only point at which that is observable.
void pin_console_activation_policy();

} // namespace singularity::platform
