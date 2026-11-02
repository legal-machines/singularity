#include "Node/DeviceCapabilities/DeviceCapabilities.hpp"

#include <algorithm>

namespace singularity::hardware {

bool HardwareCapabilities::supports(ExecutionBackend backend) const {
  return std::find(available_backends.begin(), available_backends.end(), backend) !=
         available_backends.end();
}

const char *to_string(MemoryPressureLevel level) {
	switch (level) {
		case MemoryPressureLevel::Unknown: return "unknown";
		case MemoryPressureLevel::Normal: return "normal";
		case MemoryPressureLevel::Warning: return "warning";
		case MemoryPressureLevel::Critical: return "critical";
	}
	return "unknown";
}

} // namespace singularity::hardware
