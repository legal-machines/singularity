#include "Platform/AcceleratorCalibration/AcceleratorCalibration.hpp"

namespace singularity::platform {

AcceleratorCapabilities inspect_accelerator_capabilities() {
	return {};
}

AcceleratorCalibration measure_accelerator_calibration() {
	AcceleratorCalibration result;
	result.unavailable_reason = "This platform adapter does not expose an accelerator calibration path.";
	return result;
}

} // namespace singularity::platform
