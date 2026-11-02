#include "Platform/ProcessMetrics/ProcessMetrics.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#else
#include <sys/resource.h>
#endif

namespace singularity::platform {

std::uint64_t peak_resident_memory_bytes() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return 0;
  return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
#elif defined(__APPLE__)
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
      KERN_SUCCESS) {
    return 0;
  }
  // resident_size_max is already bytes on Darwin, unlike Linux's ru_maxrss.
  return static_cast<std::uint64_t>(info.resident_size_max);
#else
  struct rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
  // Linux reports ru_maxrss in kilobytes, not bytes.
  return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
}

} // namespace singularity::platform
