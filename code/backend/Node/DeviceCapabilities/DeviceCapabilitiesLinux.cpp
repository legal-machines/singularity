#include "Node/DeviceCapabilities/DeviceCapabilities.hpp"

#include "LocalInference/RuntimeClosure/RuntimeClosure.hpp"
#include "Platform/CudaDriver/CudaDriver.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace singularity::hardware {
namespace {

// Reads one "<Key>:   <kB> kB" line from /proc/meminfo - the standard,
// dependency-free way to read memory figures on Linux, and the only source
// for MemAvailable below.
std::optional<std::uint64_t> proc_meminfo_kilobytes_field(const std::string &key) {
	std::ifstream meminfo("/proc/meminfo");
	std::string line;
	const auto prefix = key + ":";
	while (std::getline(meminfo, line)) {
		if (line.rfind(prefix, 0) != 0) continue;
		std::istringstream stream(line.substr(prefix.size()));
		std::uint64_t kilobytes = 0;
		if (stream >> kilobytes) return kilobytes;
		break;
	}
	return std::nullopt;
}

std::string trim(std::string value) {
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return {};
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

std::string read_first_line(const std::filesystem::path &path) {
	std::ifstream input(path);
	std::string line;
	std::getline(input, line);
	return trim(line);
}

// The first "<key>: value" of /proc/cpuinfo.
std::string cpuinfo_field(const std::string &key) {
	std::ifstream cpuinfo("/proc/cpuinfo");
	std::string line;
	while (std::getline(cpuinfo, line)) {
		const auto colon = line.find(':');
		if (colon == std::string::npos || trim(line.substr(0, colon)) != key) continue;
		return trim(line.substr(colon + 1));
	}
	return {};
}

// Cores, not hardware threads: distinct (package, core) pairs across the
// online CPUs' topology.
unsigned physical_core_count() {
	std::set<std::pair<std::string, std::string>> cores;
	std::error_code error;
	for (const auto &entry : std::filesystem::directory_iterator("/sys/devices/system/cpu", error)) {
		const std::string name = entry.path().filename().string();
		if (name.rfind("cpu", 0) != 0 || name.size() == 3 ||
			!std::all_of(name.begin() + 3, name.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
		const auto topology = entry.path() / "topology";
		const std::string package = read_first_line(topology / "physical_package_id");
		const std::string core = read_first_line(topology / "core_id");
		if (!core.empty()) cores.emplace(package, core);
	}
	return static_cast<unsigned>(cores.size());
}

void add_cpu_capabilities(HardwareCapabilities &capabilities) {
	std::istringstream flags(" " + cpuinfo_field("flags") + " " + cpuinfo_field("Features") + " ");
	std::set<std::string> present;
	for (std::string flag; flags >> flag;) present.insert(flag);
	const std::pair<const char *, const char *> known[] = {{"sse", "sse"}, {"sse2", "sse2"}, {"pni", "sse3"},
		{"ssse3", "ssse3"}, {"sse4_1", "sse4.1"}, {"sse4_2", "sse4.2"}, {"avx", "avx"}, {"avx2", "avx2"},
		{"avx512f", "avx512f"}, {"asimd", "neon"}, {"asimddp", "dotprod"}};
	for (const auto &[flag, name] : known) {
		if (present.count(flag) != 0) capabilities.cpu_capabilities.emplace_back(name);
	}
}

} // namespace

HardwareCapabilities detect_hardware_capabilities() {
	HardwareCapabilities capabilities;
	const auto online_cores = sysconf(_SC_NPROCESSORS_ONLN);
	capabilities.logical_cores = online_cores > 0 ? static_cast<unsigned>(online_cores) : 0;
	capabilities.physical_cores = physical_core_count();
	if (capabilities.physical_cores == 0 || capabilities.physical_cores > capabilities.logical_cores)
		capabilities.physical_cores = capabilities.logical_cores;
	capabilities.cpu_identity = cpuinfo_field("model name");
	capabilities.hardware_model = trim(read_first_line("/sys/class/dmi/id/sys_vendor") + " " +
		read_first_line("/sys/class/dmi/id/product_name"));
	utsname system{};
	capabilities.operating_system_identity = uname(&system) == 0
		? std::string("Linux ") + system.release : std::string("Linux");
	add_cpu_capabilities(capabilities);

	// MemAvailable (kernel-computed since Linux 3.14) is the honest "usable
	// without swapping" figure - unlike MemFree, it already accounts for
	// reclaimable caches the kernel would give back to a new allocation.
	capabilities.total_system_memory_bytes = proc_meminfo_kilobytes_field("MemTotal").value_or(0) * 1024;
	const auto available = proc_meminfo_kilobytes_field("MemAvailable");
	if (available) capabilities.available_system_memory_bytes = *available * 1024;
	if (available && capabilities.total_system_memory_bytes > 0) {
		// The same load thresholds as every other platform: 75% warning,
		// 90% critical.
		const auto load_percent = 100 - (*capabilities.available_system_memory_bytes * 100 /
			capabilities.total_system_memory_bytes);
		capabilities.memory_pressure = load_percent < 75 ? MemoryPressureLevel::Normal
			: load_percent < 90 ? MemoryPressureLevel::Warning : MemoryPressureLevel::Critical;
	}

	// The runtimes this build carries: the pinned ONNX Runtime's CPU build that
	// Papinian NER runs on, and local inference's llama.cpp runtime.
	capabilities.runtime_identities = {"onnxruntime:1.23.2:cpu"};
	const local_inference::RuntimeClosureInfo &closure = local_inference::packaged_runtime_closure();
	if (closure.packaged) capabilities.runtime_identities.push_back(closure.identity);
	capabilities.available_backends = {ExecutionBackend::Cpu};
	if (closure.accelerator == ExecutionBackend::Cuda) {
		if (const auto driver = platform::cuda::query_driver()) {
			const auto discrete = std::find_if(driver->devices.begin(), driver->devices.end(),
				[](const platform::cuda::Device &device) { return !device.integrated; });
			const platform::cuda::Device &device = discrete != driver->devices.end() ? *discrete : driver->devices.front();
			capabilities.accelerator_identity = device.name;
			capabilities.unified_memory = device.integrated;
			capabilities.accelerator_memory_bytes = device.total_memory_bytes;
			// Free device memory is the budget a new runtime can count on;
			// without the management library it stays unknown, and the
			// planner then places nothing on the device.
			capabilities.accelerator_memory_budget_bytes = platform::cuda::free_memory_bytes(device);
			capabilities.available_backends.insert(capabilities.available_backends.begin(), ExecutionBackend::Cuda);
		}
	}
	return capabilities;
}

} // namespace singularity::hardware
