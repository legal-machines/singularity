#pragma once

#include <string>

namespace singularity::platform {

// Asks the system to keep Singularity working at its full pace while its
// window is in the background, hidden or covered: AI colleagues work there
// and the firm's coordination goes round every two seconds, while the lawyer
// works in Word. Left alone, macOS puts a background app to App Nap and
// Windows throttles it to save power (EcoQoS), and both slowed the
// colleagues and the exchange to a crawl (found on the Mac, 2026-10-25: every
// thread at background priority, the window's reads waiting minutes).
// - macOS: NSProcessInfo's beginActivityWithOptions with
//   NSActivityUserInitiatedAllowingIdleSystemSleep, held for the rest of the
//   process (BackgroundActivityMacOS.mm); the computer may still sleep when
//   idle.
// - Windows: the process opts out of power throttling's execution-speed
//   throttling (SetProcessInformation, ProcessPowerThrottling)
//   (BackgroundActivityWindows.cpp).
// - Linux: nothing slows a background process so; nothing to ask
//   (BackgroundActivityLinux.cpp).
// Called once when the window starts its background services; a later call
// does nothing more. Returns whether the system holds the request: false
// where there is nothing to ask, or the system refused it.
bool keep_working_in_background(const std::string& reason);

} // namespace singularity::platform
