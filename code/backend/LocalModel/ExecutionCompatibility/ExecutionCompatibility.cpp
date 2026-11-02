#include "LocalModel/ExecutionCompatibility/ExecutionCompatibility.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace singularity::hardware {
namespace {

std::string format_gib(std::uint64_t bytes) {
  std::ostringstream stream;
  stream.precision(1);
  stream << std::fixed << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << " GiB";
  return stream.str();
}

bool is_accelerator(ExecutionBackend backend) { return backend != ExecutionBackend::Cpu; }

// Empty when the backend fits; otherwise the plain-language reason it does
// not, e.g. "insufficient accelerator memory: needs 4.0 GiB, 2.0 GiB
// available".
std::string unfit_reason(ExecutionBackend backend, const ResourceRequirement &requirement,
                         const HardwareCapabilities &hardware) {
  if (!hardware.supports(backend)) {
    return std::string(to_string(backend)) + " is not available in this build on this platform";
  }
  if (requirement.min_ram_bytes > 0 &&
      hardware.total_system_memory_bytes < requirement.min_ram_bytes) {
    return "insufficient system memory: needs " + format_gib(requirement.min_ram_bytes) + ", " +
           format_gib(hardware.total_system_memory_bytes) + " available";
  }
  if (is_accelerator(backend) && requirement.min_accelerator_memory_bytes > 0) {
    // Not knowing the accelerator's memory is treated the same as not
    // having enough - "hardware GPU exists" is never treated as "usable"
    // without an actual figure to check against.
    if (!hardware.accelerator_memory_bytes ||
        *hardware.accelerator_memory_bytes < requirement.min_accelerator_memory_bytes) {
      const auto available = hardware.accelerator_memory_bytes
                                  ? format_gib(*hardware.accelerator_memory_bytes)
                                  : std::string("unknown");
      return "insufficient accelerator memory: needs " +
             format_gib(requirement.min_accelerator_memory_bytes) + ", " + available +
             " available";
    }
  }
  return "";
}

} // namespace

ExecutionPlan choose_execution_plan(const std::vector<ExecutionBackend> &preferred_backends,
                                     const ResourceRequirement &requirement,
                                     const HardwareCapabilities &hardware,
                                     std::optional<ExecutionBackend> forced) {
  if (forced) {
    if (std::find(preferred_backends.begin(), preferred_backends.end(), *forced) ==
        preferred_backends.end()) {
      throw std::invalid_argument(std::string("this model does not support ") +
                                  to_string(*forced));
    }
    const auto reason = unfit_reason(*forced, requirement, hardware);
    if (!reason.empty()) {
      throw std::invalid_argument(std::string(to_string(*forced)) + " was requested but " +
                                  reason);
    }
    return ExecutionPlan{*forced, "forced by user setting", /*forced=*/true};
  }

  for (const auto backend : preferred_backends) {
    const auto reason = unfit_reason(backend, requirement, hardware);
    if (reason.empty()) {
      return ExecutionPlan{
          backend,
          backend == preferred_backends.front()
              ? "preferred backend for this model on this machine"
              : "next viable backend after higher-preference options did not fit",
          /*forced=*/false,
      };
    }
  }

  throw std::runtime_error(
      "no execution backend is available for this model on this build/platform");
}

CompatibilityAssessment assess_compatibility(const std::vector<ExecutionBackend> &preferred_backends,
                                              const ResourceRequirement &requirement,
                                              const HardwareCapabilities &hardware) {
  for (std::size_t index = 0; index < preferred_backends.size(); ++index) {
    const auto backend = preferred_backends[index];
    const auto reason = unfit_reason(backend, requirement, hardware);
    if (reason.empty()) {
      if (index == 0) {
        return {CompatibilityLevel::Recommended,
                std::string("runs on ") + to_string(backend) + ", the preferred backend for this model on this machine"};
      }
      return {CompatibilityLevel::WillRunSlowly,
              std::string("runs on ") + to_string(backend) + " after higher-preference options did not fit"};
    }
  }
  if (preferred_backends.empty()) {
    return {CompatibilityLevel::Unsupported, "this model format or architecture has no installed execution runtime"};
  }
  return {CompatibilityLevel::NotRecommended,
          unfit_reason(preferred_backends.front(), requirement, hardware)};
}

std::uint64_t runtime_memory_budget_bytes(const HardwareCapabilities &hardware) {
  return audit_runtime_memory_budget(hardware).effective_budget_bytes;
}

RuntimeMemoryBudgetAudit audit_runtime_memory_budget(const HardwareCapabilities &hardware) {
  RuntimeMemoryBudgetAudit result;
  result.total_physical_bytes = hardware.total_system_memory_bytes;
  result.os_available_bytes = hardware.available_system_memory_bytes;
  result.memory_pressure = hardware.memory_pressure;
  if (!hardware.available_system_memory_bytes) return result;

  // Reserve the smaller of 2 GiB or a fifth of physical memory for the OS,
  // the rest of the user's work and allocation/measurement error - a local
  // model must never be allowed to claim literally every free byte on the
  // machine. A responsiveness policy from machine facts alone: no model,
  // candidate plan or benchmark result can alter it.
  constexpr std::uint64_t kMaxReserveBytes = 2ULL * 1024 * 1024 * 1024;
  constexpr std::uint64_t kMaximumSensitivityDelta = 512ULL * 1024 * 1024;
  result.responsiveness_reserve_bytes = hardware.total_system_memory_bytes > 0
      ? std::min(kMaxReserveBytes, hardware.total_system_memory_bytes / 5) : 0;
  const auto sensitivity_delta = std::min(result.responsiveness_reserve_bytes / 4, kMaximumSensitivityDelta);
  result.tighter_reserve_bytes = result.responsiveness_reserve_bytes + sensitivity_delta;
  result.looser_reserve_bytes = result.responsiveness_reserve_bytes - sensitivity_delta;

  const auto available = *hardware.available_system_memory_bytes;
  const auto budget_after_reserve = [available](std::uint64_t reserve) {
    return available > reserve ? available - reserve : 0;
  };
  result.normal_pressure_budget_bytes = budget_after_reserve(result.responsiveness_reserve_bytes);
  result.tighter_budget_bytes = budget_after_reserve(result.tighter_reserve_bytes);
  result.looser_budget_bytes = budget_after_reserve(result.looser_reserve_bytes);

  // Warning halves what may be claimed; Critical allows nothing.
  const auto apply_pressure = [&hardware](std::uint64_t budget) {
    if (hardware.memory_pressure == MemoryPressureLevel::Critical) return std::uint64_t{0};
    return hardware.memory_pressure == MemoryPressureLevel::Warning ? budget / 2 : budget;
  };
  result.effective_budget_bytes = apply_pressure(result.normal_pressure_budget_bytes);
  result.tighter_budget_bytes = apply_pressure(result.tighter_budget_bytes);
  result.looser_budget_bytes = apply_pressure(result.looser_budget_bytes);
  return result;
}

LoadImpact assess_load_impact(const ResourceRequirement &requirement, const HardwareCapabilities &hardware) {
  LoadImpact result;
  result.expected_bytes =
      requirement.recommended_ram_bytes > 0 ? requirement.recommended_ram_bytes : requirement.min_ram_bytes;

  if (!hardware.available_system_memory_bytes) {
    result.level = CompatibilityLevel::NotRecommended;
    result.reason = "current available memory on this machine is unknown";
    return result;
  }

  const auto budget = runtime_memory_budget_bytes(hardware);
  if (requirement.min_ram_bytes > 0 && budget < requirement.min_ram_bytes) {
    result.level = CompatibilityLevel::NotRecommended;
    result.reason = "this model needs more memory than is safely available right now";
    return result;
  }
  if (requirement.recommended_ram_bytes > 0 && budget < requirement.recommended_ram_bytes) {
    result.level = CompatibilityLevel::WillRunSlowly;
    result.reason = "loading now will create a noticeable memory load on this machine";
    return result;
  }
  result.level = CompatibilityLevel::Recommended;
  result.reason = "enough memory is safely available right now";
  return result;
}

} // namespace singularity::hardware
