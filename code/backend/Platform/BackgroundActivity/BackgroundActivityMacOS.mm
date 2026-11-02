#include "Platform/BackgroundActivity/BackgroundActivity.hpp"

#import <Foundation/Foundation.h>

#include <mutex>

namespace singularity::platform {

bool keep_working_in_background(const std::string& reason) {
	// The activity is held for the rest of the process: App Nap stays off and
	// timers keep their pace, while the computer may still sleep when idle.
	static std::once_flag once;
	static id activity = nil;
	std::call_once(once, [&reason] {
		@autoreleasepool {
			NSString* why = [NSString stringWithUTF8String:reason.c_str()];
			if (why == nil) why = @"AI colleagues and the firm's coordination work in the background";
			// Not under ARC: the activity token is retained so it lives as long as
			// the process.
			activity = [[[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep
				reason:why] retain];
		}
	});
	return activity != nil;
}

} // namespace singularity::platform
