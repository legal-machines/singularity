// Keeps Singularity's command-line and test executables out of the Dock.
//
// These binaries link AppKit indirectly (Papinian's CoreML path, NSURLSession,
// the Security framework). The moment AppKit initialises, macOS promotes the
// process to a regular foreground application and gives it a Dock tile - so a
// plain `ctest` run, or one `singularity_cli` session, made nameless "exec" icons
// appear in the Dock.
//
// Two simpler mechanisms were tried first and measured, on this machine,
// against `lsappinfo info <asn>`'s own reported process type:
//
//   * An embedded LSBackgroundOnly/LSUIElement Info.plist section
//     (__TEXT,__info_plist). LaunchServices honours those when it LAUNCHES a
//     bundled application, not when a bare executable is fork/exec'd from a
//     shell - which is how every one of these binaries starts. The section is
//     still embedded (see singularity_add_executable in CMakeLists.txt) so the
//     intent is declared where a reader looks for it, but it does not enforce
//     anything on its own.
//   * TransformProcessType(kProcessTransformToUIElementApplication) from a
//     pre-main constructor. It returns noErr, but AppKit's own later
//     initialisation promotes the process back: the real binary still measured
//     as type="Foreground".
//
// What actually holds is initialising AppKit HERE, deterministically, and
// immediately pinning the accessory (UIElement) policy - measured as
// type="UIElement", which has no Dock tile. Doing it from a constructor means
// this ordering is ours rather than incidental: by the time any other code
// touches AppKit, the policy is already set. These processes were initialising
// AppKit anyway - that is precisely why they appeared - so this moves the cost
// rather than adding it.
//
// NSApplicationActivationPolicyProhibited is deliberately NOT used: it is
// rejected once a process is already a regular application (measured: setter
// returned NO), whereas Accessory is accepted.
//
// Measured result: ordinary test binaries stop registering with LaunchServices
// entirely (no Dock tile). One target, long_horizon_replay_runner, still
// measures as type="Foreground" - something it initialises promotes the
// process back, and re-asserting the policy from its own main() was tried and
// did NOT hold either, so the cause is later still and is left as a known,
// cosmetic exception rather than guessed at.
//
// The real application is a MACOSX_BUNDLE target and is excluded by
// singularity_add_executable - it must keep its Dock presence.
#if defined(__APPLE__)

#include "Platform/ConsoleActivationPolicy/ConsoleActivationPolicy.hpp"

#import <AppKit/AppKit.h>

namespace singularity::platform {

void pin_console_activation_policy() {
	@autoreleasepool {
		[NSApplication sharedApplication];
		// Best effort by design: a failure here must never stop a console tool
		// or a test from running - the worst case is the cosmetic Dock tile
		// this exists to avoid.
		(void)[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
	}
}

} // namespace singularity::platform

namespace {

__attribute__((constructor)) void singularity_suppress_dock_presence() {
	singularity::platform::pin_console_activation_policy();
}

} // namespace

#endif
