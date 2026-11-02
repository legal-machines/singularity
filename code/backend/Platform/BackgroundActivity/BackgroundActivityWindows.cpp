#include "Platform/BackgroundActivity/BackgroundActivity.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mutex>

namespace singularity::platform {

bool keep_working_in_background(const std::string&) {
	// Power throttling (EcoQoS) would run a background Singularity on the
	// slowest cores at the lowest clock: the process says it is not to be
	// throttled for execution speed, for the rest of its life.
	static std::once_flag once;
	static bool isHeld = false;
	std::call_once(once, [] {
		PROCESS_POWER_THROTTLING_STATE state{};
		state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
		state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
		state.StateMask = 0;
		isHeld = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)) != FALSE;
	});
	return isHeld;
}

} // namespace singularity::platform
