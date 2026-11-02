#include "Node/NodeSummary/NodeSummary.hpp"

#include "LocalModel/LocalModelRegistry/LocalModelRegistry.hpp"
#include "Node/DeviceCapabilities/DeviceCapabilities.hpp"
#include "Node/HardwareCalibration/HardwareCalibration.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/AppPaths/AppPaths.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#else
#include <unistd.h>
#endif

namespace singularity::node {
namespace {

std::uint64_t recursive_directory_size(const std::filesystem::path &path) {
  std::error_code error;
  if (!std::filesystem::is_directory(path, error) || error) return 0;
  std::uint64_t total = 0;
  const auto options = std::filesystem::directory_options::skip_permission_denied;
  for (std::filesystem::recursive_directory_iterator iterator(path, options, error), end;
       iterator != end; iterator.increment(error)) {
    if (error) { error.clear(); continue; }
    std::error_code entry_error;
    if (iterator->is_symlink(entry_error) || entry_error) continue;
    if (iterator->is_regular_file(entry_error) && !entry_error) total += iterator->file_size(entry_error);
  }
  return total;
}

std::uint64_t regular_files_size(const std::filesystem::path &path) {
  std::error_code error;
  if (!std::filesystem::is_directory(path, error) || error) return 0;
  std::uint64_t total = 0;
  for (std::filesystem::directory_iterator iterator(path, error), end;
       iterator != end; iterator.increment(error)) {
    if (error) { error.clear(); continue; }
    std::error_code entry_error;
    if (iterator->is_regular_file(entry_error) && !entry_error) total += iterator->file_size(entry_error);
  }
  return total;
}

std::optional<std::string> device_name() {
#if defined(_WIN32)
  char name[256]{};
  DWORD size = static_cast<DWORD>(sizeof(name));
  return GetComputerNameA(name, &size) && size > 0 ? std::optional(std::string(name, size)) : std::nullopt;
#elif defined(__APPLE__)
  CFStringRef computer_name = SCDynamicStoreCopyComputerName(nullptr, nullptr);
  if (!computer_name) return std::nullopt;
  char name[512]{};
  const bool converted = CFStringGetCString(computer_name, name, sizeof(name), kCFStringEncodingUTF8);
  CFRelease(computer_name);
  return converted && name[0] != '\0' ? std::optional(std::string(name)) : std::nullopt;
#else
  char name[256]{};
  return gethostname(name, sizeof(name) - 1) == 0 && name[0] != '\0'
             ? std::optional(std::string(name)) : std::nullopt;
#endif
}

const char *operating_system_label() {
#if defined(__APPLE__)
  return "macOS";
#elif defined(_WIN32)
  return "Windows";
#else
  return "Linux";
#endif
}

// This machine's Hardware Profile abstraction lives in one directory under
// application data, shared by the calibration cache and (see
// Routing/InferenceExecutor/InferenceExecutor.cpp) any model's own resource
// benchmark baseline - never network state, never a Project/Matter path.
std::filesystem::path hardware_profile_directory() {
  return platform::application_data_directory() / "HardwareProfile";
}

} // namespace

NodeSummary get_node_summary() {
  NodeSummary summary;
  summary.device_name = device_name();
  summary.operating_system = operating_system_label();

  const auto capabilities = hardware::detect_hardware_capabilities();
	summary.operating_system_identity = capabilities.operating_system_identity;
	summary.hardware_model = capabilities.hardware_model;
	summary.memory_pressure = hardware::to_string(capabilities.memory_pressure);
	summary.runtime_identities = capabilities.runtime_identities;
	if (!capabilities.cpu_identity.empty()) summary.cpu.identity = capabilities.cpu_identity;
	summary.cpu.capabilities = capabilities.cpu_capabilities;
  if (capabilities.physical_cores > 0) summary.cpu.physical_cores = capabilities.physical_cores;
  if (capabilities.logical_cores > 0) summary.cpu.logical_cores = capabilities.logical_cores;
  if (capabilities.total_system_memory_bytes > 0) summary.cpu.total_memory_bytes = capabilities.total_system_memory_bytes;
  summary.cpu.available_memory_bytes = capabilities.available_system_memory_bytes;
  for (const auto backend : capabilities.available_backends) summary.available_backends.emplace_back(hardware::to_string(backend));

  // The first non-Cpu backend this build/machine can actually use, if any -
  // Cpu is always the universal fallback, never itself "the accelerator".
  for (const auto backend : capabilities.available_backends) {
    if (backend == hardware::ExecutionBackend::Cpu) continue;
    summary.accelerator.backend = hardware::to_string(backend);
    break;
  }
  summary.accelerator.memory_bytes = capabilities.accelerator_memory_bytes;
	if (!capabilities.accelerator_identity.empty()) summary.accelerator.identity = capabilities.accelerator_identity;
	summary.accelerator.memory_budget_bytes = capabilities.accelerator_memory_budget_bytes;
	summary.accelerator.unified_memory = capabilities.unified_memory;

  // Measured once per real hardware signature and cached - see
  // Node/HardwareCalibration/HardwareCalibration.hpp's "short, local and
  // cached, repeated only when necessary" contract. Cheap enough (well
  // under a second) to compute on demand here rather than gating it behind
  // a separate startup step.
  const auto calibration = hardware::cached_hardware_calibration(
      hardware_profile_directory() / "calibration.json", hardware_profile_directory(), capabilities);
  summary.calibration.memory_bandwidth_bytes_per_second = calibration.memory_bandwidth_bytes_per_second;
  summary.calibration.storage_read_bytes_per_second = calibration.storage_read_bytes_per_second;
  summary.calibration.cpu_throughput_score = calibration.cpu_throughput_score;
	summary.calibration.cpu_execution_giga_operations_per_second = calibration.cpu_execution_giga_operations_per_second;
	summary.calibration.accelerator_memory_bandwidth_bytes_per_second = calibration.accelerator_memory_bandwidth_bytes_per_second;
	summary.calibration.accelerator_execution_giga_operations_per_second = calibration.accelerator_execution_giga_operations_per_second;
	summary.calibration.accelerator_command_overhead_microseconds = calibration.accelerator_command_overhead_microseconds;
	summary.calibration.host_to_accelerator_bandwidth_bytes_per_second = calibration.host_to_accelerator_bandwidth_bytes_per_second;
	summary.calibration.host_to_accelerator_semantics = calibration.host_to_accelerator_semantics;
	summary.calibration.accelerator_calibration_unavailable_reason = calibration.accelerator_calibration_unavailable_reason;
  summary.calibration.measured_at_epoch_seconds = calibration.measured_at_epoch_seconds;

  for (const auto &status : routing::production_local_model_registry().list_status()) {
    summary.storage.local_models_bytes += status.disk_bytes;
    if (status.live.load_state == hardware::LocalModelLoadState::Ready) summary.loaded_local_model_ids.push_back(status.descriptor.id);
  }

  std::uint64_t workspace_node_bytes = 0;
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    const auto settings = platform::load_app_settings();
    const auto &setup = settings.local_node_setup;
    if (setup.active_workspace_id) {
      const auto found = std::find_if(setup.workspaces.begin(), setup.workspaces.end(),
          [&](const platform::LocalWorkspace &workspace) { return workspace.id == *setup.active_workspace_id; });
      if (found != setup.workspaces.end() && workspaces::directory_available(found->path)) {
        const auto root = std::filesystem::path(found->path) / ".singularity-workspace";
        summary.storage.matter_documents_bytes = recursive_directory_size(root / "documents");
        summary.storage.indexes_cache_bytes = recursive_directory_size(root / "derivatives");
        summary.storage.matter_state_bytes = regular_files_size(root);
        workspace_node_bytes = recursive_directory_size(root);
      }
    }
  }
  summary.storage.total_bytes = summary.storage.local_models_bytes + workspace_node_bytes;
  return summary;
}

} // namespace singularity::node
