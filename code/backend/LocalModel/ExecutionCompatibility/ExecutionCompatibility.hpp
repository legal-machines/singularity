#pragma once

#include "LocalModel/ExecutionBackend/ExecutionBackend.hpp"
#include "Node/DeviceCapabilities/DeviceCapabilities.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The execution-backend and resource-compatibility policy every local module
// shares: "which backend, and how well" for any locally registered model
// (Papinian's GLiNER2 encoder, a local generative model) from plain,
// already-detected hardware facts, and the memory budget all local work is
// held to. It contains no execution planning for a generative model (dense
// and MoE strategies, expert residency, calibration-driven placement): that
// is local inference's own planner (LocalInference/ExecutionPlanner), which
// builds on the types and the budget defined here.
namespace singularity::hardware {

// What a local model needs to run at all, and to run well - declared by the
// model, not guessed by the planner. Figures are deliberately coarse
// (rounded, measured-evidence-based) rather than exact.
struct ResourceRequirement {
  std::uint64_t min_ram_bytes = 0;
  std::uint64_t recommended_ram_bytes = 0;
  // Only consulted for accelerator backends (AppleAccelerator/Cuda/
  // DirectMl); 0 means "no meaningful accelerator memory floor is known".
  std::uint64_t min_accelerator_memory_bytes = 0;
};

struct ExecutionPlan {
  ExecutionBackend backend;
  // Always populated, even for the obvious/default choice - "why" is what
  // Settings and support conversations actually need, not just "what".
  std::string reason;
  bool forced = false;
};

// Chooses the backend expected to work, and work well, for one model on one
// machine. Pure and hardware-independent (HardwareCapabilities is a plain
// value the caller already detected), so it is fully unit-testable with
// synthetic hardware/requirement combinations without needing a real GPU, a
// real Windows/Linux box, or a real model.
//
// `preferred_backends` is the model's own ordered preference (e.g. Papinian:
// {AppleAccelerator, Cpu}) - the planner never invents a preference order a
// model didn't declare.
//
// `forced`, when set, must be honored or explicitly rejected: throws
// std::invalid_argument with a plain-language reason if the forced backend
// is not in `preferred_backends`, not in `hardware.available_backends`, or
// (for an accelerator) the hardware's accelerator memory is below
// `requirement.min_accelerator_memory_bytes`. Never silently substitutes a
// different backend for an explicit forced choice.
//
// In Auto mode (forced = nullopt), tries each of the model's preferred
// backends in order, skipping any not in hardware.available_backends or
// whose memory floor the hardware does not meet, and returns the first that
// fits. Throws std::runtime_error if literally nothing fits.
ExecutionPlan choose_execution_plan(const std::vector<ExecutionBackend> &preferred_backends,
                                     const ResourceRequirement &requirement,
                                     const HardwareCapabilities &hardware,
                                     std::optional<ExecutionBackend> forced = std::nullopt);

// The pre-install compatibility signal Model Library cards show instead of a
// quantization/backend list.
enum class CompatibilityLevel { Recommended, WillRunSlowly, NotRecommended, Unsupported };

struct CompatibilityAssessment {
  CompatibilityLevel level = CompatibilityLevel::NotRecommended;
  // Always populated, plain-language - why this level, not just what it is.
  std::string reason;
};

// A never-throwing superset of choose_execution_plan()'s decision: the same
// preferred-backend-in-order search, but "nothing fits" is a valid
// NotRecommended result here rather than an exception, since a card must be
// able to show compatibility before a model is ever installed or chosen.
CompatibilityAssessment assess_compatibility(const std::vector<ExecutionBackend> &preferred_backends,
                                              const ResourceRequirement &requirement,
                                              const HardwareCapabilities &hardware);

// The memory Singularity will actually let itself use for local models right
// now: real currently-available system memory minus a safety reserve for
// the OS and the rest of the user's work - never the raw total, and never
// literally every free byte. Returns 0 when available memory could not be
// measured on this platform (honest, not a guess).
std::uint64_t runtime_memory_budget_bytes(const HardwareCapabilities &hardware);

// Model-independent audit of that budget: how it was reached from this
// machine's facts, with the reserve varied by +/-25% (at most 512 MiB) to show
// its sensitivity. Requirements, candidate plans and benchmark scores are
// absent by design, so every candidate is held to the same boundary.
// runtime_memory_budget_bytes() is its effective_budget_bytes.
struct RuntimeMemoryBudgetAudit {
	std::uint64_t total_physical_bytes = 0;
	std::optional<std::uint64_t> os_available_bytes;
	MemoryPressureLevel memory_pressure = MemoryPressureLevel::Unknown;
	std::uint64_t responsiveness_reserve_bytes = 0;
	std::uint64_t normal_pressure_budget_bytes = 0;
	std::uint64_t effective_budget_bytes = 0;
	std::uint64_t tighter_reserve_bytes = 0;
	std::uint64_t tighter_budget_bytes = 0;
	std::uint64_t looser_reserve_bytes = 0;
	std::uint64_t looser_budget_bytes = 0;
};

RuntimeMemoryBudgetAudit audit_runtime_memory_budget(const HardwareCapabilities &hardware);

// What loading one already-installed model right now, on this machine,
// would cost against the live runtime memory budget above - distinct from
// assess_compatibility(), which only checks the model's declared
// requirement against total memory before it is ever installed. Reuses the
// same three-tier vocabulary so both signals read the same way on a card.
struct LoadImpact {
  // The memory this load is expected to add - requirement.recommended_ram_bytes,
  // falling back to min_ram_bytes when no recommended figure is declared.
  std::uint64_t expected_bytes = 0;
  CompatibilityLevel level = CompatibilityLevel::NotRecommended;
  std::string reason;
};

LoadImpact assess_load_impact(const ResourceRequirement &requirement, const HardwareCapabilities &hardware);

} // namespace singularity::hardware
