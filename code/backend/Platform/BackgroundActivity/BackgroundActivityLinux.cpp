#include "Platform/BackgroundActivity/BackgroundActivity.hpp"

namespace singularity::platform {

bool keep_working_in_background(const std::string&) {
	// Linux has no App Nap and no power throttling of a background process:
	// nothing to ask.
	return false;
}

} // namespace singularity::platform
