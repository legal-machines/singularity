#include "Node/DeviceCapabilities/DeviceCapabilities.hpp"

#include "LocalInference/RuntimeClosure/RuntimeClosure.hpp"
#include "Platform/CudaDriver/CudaDriver.hpp"
#include "Platform/WindowsText/WindowsText.hpp"

#include <windows.h>
#include <dxgi1_4.h>
#include <psapi.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace singularity::hardware {
namespace {

std::string trim(std::string value) {
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return {};
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

std::string registry_string(const wchar_t *subkey, const wchar_t *name) {
	DWORD bytes = 0;
	if (RegGetValueW(HKEY_LOCAL_MACHINE, subkey, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
		bytes < sizeof(wchar_t)) return {};
	std::wstring value(bytes / sizeof(wchar_t), L'\0');
	if (RegGetValueW(HKEY_LOCAL_MACHINE, subkey, name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
		return {};
	value.resize(wcsnlen(value.c_str(), value.size()));
	return trim(platform::windows::wide_to_utf8(value));
}

// Cores, not hardware threads: one RelationProcessorCore record per core,
// across every processor group.
unsigned physical_core_count() {
	DWORD bytes = 0;
	(void)GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
	if (bytes == 0) return 0;
	std::vector<unsigned char> buffer(bytes);
	if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
		reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &bytes)) return 0;
	// Records are variable-length - each carries its own Size, usually far
	// smaller than sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)'s union -
	// so the walk is bounded by each record's header and Size, never by the
	// struct's size (which would silently drop the last core).
	constexpr DWORD header_bytes = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor);
	unsigned cores = 0;
	for (DWORD offset = 0; offset + header_bytes <= bytes;) {
		const auto *entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data() + offset);
		if (entry->Size < header_bytes || offset + entry->Size > bytes) break;
		if (entry->Relationship == RelationProcessorCore) ++cores;
		offset += entry->Size;
	}
	return cores;
}

std::string operating_system_identity() {
	// RtlGetVersion reports the real version; GetVersionEx is shimmed to
	// whatever the executable's manifest declares.
	using RtlGetVersion = LONG(WINAPI *)(OSVERSIONINFOW *);
	const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	const auto get_version = ntdll == nullptr ? nullptr
		: reinterpret_cast<RtlGetVersion>(reinterpret_cast<void *>(GetProcAddress(ntdll, "RtlGetVersion")));
	OSVERSIONINFOW version{};
	version.dwOSVersionInfoSize = sizeof(version);
	if (get_version == nullptr || get_version(&version) != 0) return "Windows";
	return "Windows " + std::to_string(version.dwMajorVersion) + "." + std::to_string(version.dwMinorVersion) +
		"." + std::to_string(version.dwBuildNumber);
}

// What Windows reports this processor (and this process) can execute - not
// what a model name implies.
void add_cpu_capabilities(HardwareCapabilities &capabilities) {
	struct Feature {
		DWORD id;
		const char *name;
	};
#if defined(_M_ARM64)
	constexpr Feature features[] = {{19, "neon"}, {43, "dotprod"}};
#else
	constexpr Feature features[] = {{6, "sse"}, {10, "sse2"}, {13, "sse3"}, {36, "ssse3"}, {37, "sse4.1"},
		{38, "sse4.2"}, {39, "avx"}, {40, "avx2"}, {41, "avx512f"}};
#endif
	for (const Feature &feature : features) {
		if (IsProcessorFeaturePresent(feature.id)) capabilities.cpu_capabilities.emplace_back(feature.name);
	}
}

struct AdapterMemory {
	std::string identity;
	std::uint64_t dedicated_bytes = 0;
	// The OS budget for this process on the adapter's local segment, minus
	// what this process already uses - the amount a new allocation can
	// count on, not the board's capacity.
	std::optional<std::uint64_t> budget_bytes;
	std::optional<std::uint64_t> current_usage_bytes;
};

// The DXGI view of one hardware adapter: the one with `luid` when given (the
// CUDA execution device), otherwise the hardware adapter with the most
// dedicated memory. Software adapters are never accelerators.
std::optional<AdapterMemory> adapter_memory(const std::optional<std::array<std::uint8_t, 8>> &luid) {
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) return std::nullopt;
	IDXGIAdapter1 *selected = nullptr;
	DXGI_ADAPTER_DESC1 selected_description{};
	for (UINT index = 0;; ++index) {
		IDXGIAdapter1 *candidate = nullptr;
		if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
		DXGI_ADAPTER_DESC1 description{};
		const bool usable = SUCCEEDED(candidate->GetDesc1(&description)) &&
			(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0;
		bool better = false;
		if (usable && luid) {
			better = std::memcmp(&description.AdapterLuid, luid->data(), luid->size()) == 0;
		} else if (usable) {
			better = selected == nullptr || description.DedicatedVideoMemory > selected_description.DedicatedVideoMemory;
		}
		if (better) {
			if (selected != nullptr) selected->Release();
			selected = candidate;
			selected_description = description;
		} else {
			candidate->Release();
		}
	}
	factory->Release();
	if (selected == nullptr) return std::nullopt;

	AdapterMemory result;
	result.identity = platform::windows::wide_to_utf8(selected_description.Description);
	result.dedicated_bytes = selected_description.DedicatedVideoMemory;
	IDXGIAdapter3 *budget_adapter = nullptr;
	if (SUCCEEDED(selected->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void **>(&budget_adapter)))) {
		DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
		if (SUCCEEDED(budget_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
			result.current_usage_bytes = memory.CurrentUsage;
			result.budget_bytes = memory.Budget > memory.CurrentUsage ? memory.Budget - memory.CurrentUsage : 0;
		}
		budget_adapter->Release();
	}
	selected->Release();
	return result;
}

} // namespace

HardwareCapabilities detect_hardware_capabilities() {
	HardwareCapabilities capabilities;
	capabilities.logical_cores = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
	capabilities.physical_cores = physical_core_count();
	if (capabilities.physical_cores == 0 || capabilities.physical_cores > capabilities.logical_cores)
		capabilities.physical_cores = capabilities.logical_cores;
	capabilities.cpu_identity = registry_string(
		L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
	capabilities.hardware_model = trim(
		registry_string(L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemManufacturer") + " " +
		registry_string(L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemProductName"));
	capabilities.operating_system_identity = operating_system_identity();
	add_cpu_capabilities(capabilities);

	// Physical memory and commit charge are separate limits on Windows: a
	// machine can run out of commit (RAM + page file) while physical memory
	// still looks free. Pressure follows whichever is tighter; the
	// thresholds are those of every platform (75% warning, 90% critical).
	MEMORYSTATUSEX memory{};
	memory.dwLength = sizeof(memory);
	std::optional<unsigned> load_percent;
	if (GlobalMemoryStatusEx(&memory)) {
		capabilities.total_system_memory_bytes = memory.ullTotalPhys;
		capabilities.available_system_memory_bytes = memory.ullAvailPhys;
		load_percent = memory.dwMemoryLoad;
	}
	PERFORMANCE_INFORMATION performance{};
	performance.cb = sizeof(performance);
	if (GetPerformanceInfo(&performance, sizeof(performance)) && performance.CommitLimit > 0) {
		const auto commit_percent = static_cast<unsigned>(100ULL * performance.CommitTotal / performance.CommitLimit);
		load_percent = std::max(load_percent.value_or(0), commit_percent);
		capabilities.available_commit_bytes = performance.CommitLimit > performance.CommitTotal
			? static_cast<std::uint64_t>(performance.CommitLimit - performance.CommitTotal) * performance.PageSize : 0;
	}
	if (load_percent) {
		capabilities.memory_pressure = *load_percent < 75 ? MemoryPressureLevel::Normal
			: *load_percent < 90 ? MemoryPressureLevel::Warning : MemoryPressureLevel::Critical;
	}

	// The runtimes this build carries: the pinned ONNX Runtime's CPU build that
	// Papinian NER runs on, and local inference's llama.cpp runtime.
	capabilities.runtime_identities = {"onnxruntime:1.23.2:cpu"};
	const local_inference::RuntimeClosureInfo &closure = local_inference::packaged_runtime_closure();
	if (closure.packaged) capabilities.runtime_identities.push_back(closure.identity);

	// The accelerator this build can execute on (a CUDA closure needs an
	// NVIDIA driver with a device), with its memory as DXGI accounts it;
	// otherwise the most capable hardware adapter, reported as a fact but
	// never offered as an execution backend.
	std::optional<platform::cuda::Device> cuda_device;
	if (closure.accelerator == ExecutionBackend::Cuda) {
		if (const auto driver = platform::cuda::query_driver()) {
			const auto discrete = std::find_if(driver->devices.begin(), driver->devices.end(),
				[](const platform::cuda::Device &device) { return !device.integrated; });
			cuda_device = discrete != driver->devices.end() ? *discrete : driver->devices.front();
		}
	}
	const auto adapter = adapter_memory(cuda_device ? cuda_device->luid : std::nullopt);
	if (cuda_device) {
		capabilities.accelerator_identity = cuda_device->name;
		capabilities.unified_memory = cuda_device->integrated;
		capabilities.accelerator_memory_bytes = adapter && adapter->dedicated_bytes > 0
			? adapter->dedicated_bytes : cuda_device->total_memory_bytes;
	} else if (adapter) {
		capabilities.accelerator_identity = adapter->identity;
		if (adapter->dedicated_bytes > 0) capabilities.accelerator_memory_bytes = adapter->dedicated_bytes;
	}
	if (adapter && (cuda_device || adapter->dedicated_bytes > 0)) {
		capabilities.accelerator_memory_budget_bytes = adapter->budget_bytes;
		capabilities.accelerator_current_allocated_bytes = adapter->current_usage_bytes;
	}
	// WDDM backs every allocation of a discrete adapter with system commit
	// (measured: a runtime holding 15.2 GB of VRAM held 16.3 GB of commit and
	// a 0.4-0.7 GB private working set).
	capabilities.accelerator_memory_charges_commit = cuda_device && !cuda_device->integrated;

	capabilities.available_backends = {ExecutionBackend::Cpu};
	if (cuda_device) capabilities.available_backends.insert(capabilities.available_backends.begin(), ExecutionBackend::Cuda);
	return capabilities;
}

} // namespace singularity::hardware
