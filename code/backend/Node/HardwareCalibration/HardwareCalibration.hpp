#pragma once

#include "Node/DeviceCapabilities/DeviceCapabilities.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace singularity::hardware {

// Real, locally-measured facts about this machine that static OS queries
// (DeviceCapabilities.hpp) cannot report - the "sustainable host-memory
// bandwidth"/"storage read throughput"/"CPU execution cost" figures that
// hardware calibration needs. Every field here comes from an actual measurement
// taken on this process; none is estimated or looked up from a spec sheet.
struct HardwareCalibration {
	std::optional<std::uint64_t> memory_bandwidth_bytes_per_second;
	std::optional<std::uint64_t> storage_read_bytes_per_second;
	std::optional<double> cpu_execution_giga_operations_per_second;
	std::optional<std::uint64_t> accelerator_memory_bandwidth_bytes_per_second;
	std::optional<double> accelerator_execution_giga_operations_per_second;
	std::optional<double> accelerator_command_overhead_microseconds;
	// Discrete host-to-accelerator transfer does not exist on unified-memory
	// Apple Silicon. It remains null there rather than inventing a PCIe rate.
	std::optional<std::uint64_t> host_to_accelerator_bandwidth_bytes_per_second;
	std::string host_to_accelerator_semantics;
	std::string accelerator_calibration_unavailable_reason;
  // A coarse, unit-less relative CPU throughput figure from a fixed local
  // workload - comparable only against another measurement on the same
  // machine (e.g. before/after a thermal event), never an absolute FLOPS
  // claim.
	double cpu_throughput_score = 0.0;
  long long measured_at_epoch_seconds = 0;
};

// Runs a short (well under one second), local, network-free set of real
// measurements: host memory bandwidth (a timed memcpy sweep), local
// storage read throughput (write+read a temp file under
// `storage_probe_directory`) and a fixed deterministic CPU workload's
// wall-clock cost. Never fabricated. Callers should prefer
// cached_hardware_calibration() below rather than calling this on every
// launch - see the phase's own "short, local and cached, repeated only
// when necessary" calibration requirement.
HardwareCalibration measure_hardware_calibration(const std::filesystem::path &storage_probe_directory);

// A cheap, order-of-magnitude signature of this machine's calibration-
// relevant identity (core counts + total memory). Two machines - or the
// same machine before/after a real hardware change - normally differ here;
// this is what tells cached_hardware_calibration() whether to trust its
// cache or re-measure.
std::string hardware_signature(const HardwareCapabilities &capabilities);

// Returns a cached calibration from `cache_path` when its stored
// hardware_signature() still matches `capabilities`; otherwise measures a
// fresh one (via measure_hardware_calibration()), persists it to
// `cache_path` and returns it. Never throws: a cache read/write failure
// degrades to a fresh, unpersisted measurement rather than blocking the
// caller.
HardwareCalibration cached_hardware_calibration(const std::filesystem::path &cache_path,
                                                const std::filesystem::path &storage_probe_directory,
                                                const HardwareCapabilities &capabilities);

// The read-only half of cached_hardware_calibration(): the cached
// calibration when `cache_path` holds one whose hardware_signature() still
// matches `capabilities`, otherwise std::nullopt. Never measures and never
// writes - the form a capability query (planning a context budget without
// loading) must use, so asking "what would local inference plan?" can never itself
// start a hardware benchmark.
std::optional<HardwareCalibration> read_cached_hardware_calibration(
    const std::filesystem::path &cache_path, const HardwareCapabilities &capabilities);

} // namespace singularity::hardware
