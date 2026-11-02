#pragma once

#include "LocalModel/ExecutionBackend/ExecutionBackend.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace singularity::hardware {

enum class MemoryPressureLevel {
	Unknown,
	Normal,
	Warning,
	Critical,
};

// What this specific build, on this specific machine, can actually run a
// local model on - not a wishlist of what the hardware is theoretically
// capable of. `available_backends` is the honest, decision-relevant fact:
// a machine can have an NVIDIA GPU and CUDA still not be in this list,
// because this build's local inference runtime was not compiled for CUDA or
// no NVIDIA driver reports a device (see LocalInference/RuntimeClosure).
// The ONNX Runtime Papinian NER runs on is CPU-only outside macOS, and its
// model prefers the CPU wherever an accelerator is offered for local models.
struct HardwareCapabilities {
  unsigned logical_cores = 0;
  // Best-effort; equal to logical_cores where physical/logical cannot be
  // told apart on this platform.
  unsigned physical_cores = 0;
	std::uint64_t total_system_memory_bytes = 0;
	bool apple_silicon = false;
	bool unified_memory = false;
	std::string cpu_identity;
	std::string hardware_model;
	std::string accelerator_identity;
	std::string operating_system_identity;
	std::vector<std::string> cpu_capabilities;
	std::vector<std::string> runtime_identities;
	MemoryPressureLevel memory_pressure = MemoryPressureLevel::Unknown;

  // Backends this build can actually hand a session to on this machine.
  // Ordering carries no meaning; the planners consult each model's own
  // preference order, not this list's order.
  std::vector<ExecutionBackend> available_backends;

  // The accelerator's memory: on Apple silicon the unified pool (the same
  // memory as total_system_memory_bytes); on a discrete GPU its own
  // dedicated memory, with the budget a new allocation can count on right
  // now as the OS reports it (DXGI on Windows, the NVIDIA management
  // library on Linux). std::nullopt when no trustworthy figure exists.
	std::optional<std::uint64_t> accelerator_memory_bytes;
	std::optional<std::uint64_t> accelerator_memory_budget_bytes;
	std::optional<std::uint64_t> accelerator_current_allocated_bytes;
	std::optional<std::uint64_t> accelerator_recommended_working_set_bytes;
	std::optional<std::uint64_t> accelerator_maximum_transfer_bytes_per_second;
	bool supports_accelerator_residency_sets = false;
	bool supports_accelerator_storage_io = false;

  // Whether the OS charges a discrete accelerator's memory allocations to
  // system commit as well: Windows' display driver model backs each one with
  // commit so it can be evicted, so a runtime holding 15 GB of VRAM also holds
  // ~15 GB of commit. Then an accelerator plan must fit the commit still
  // available too. False where it does not apply (Linux, unified memory);
  // available_commit_bytes is std::nullopt when it cannot be measured.
	bool accelerator_memory_charges_commit = false;
	std::optional<std::uint64_t> available_commit_bytes;

  // Real currently-free system memory right now, not the fixed total above
  // - the current memory pressure signal that model load state/impact and the
  // runtime memory budget (see LocalModel/ExecutionCompatibility/
  // ExecutionCompatibility.hpp's runtime_memory_budget_bytes()) both need. std::nullopt only if this
  // platform's query genuinely failed; never guessed from the total.
  std::optional<std::uint64_t> available_system_memory_bytes;

  bool supports(ExecutionBackend backend) const;
};

// Queries the real, current machine - implemented separately per platform
// (DeviceCapabilitiesMacOS.cpp / Windows.cpp / Linux.cpp), each using only
// OS and driver APIs (sysctl and Metal; Win32 and DXGI; /proc and /sys; the
// NVIDIA driver, loaded at run time). Never throws; a detection failure degrades to a
// conservative zeroed/CPU-only result rather than crashing model warm-up.
HardwareCapabilities detect_hardware_capabilities();

const char *to_string(MemoryPressureLevel level);

} // namespace singularity::hardware
