#include "Platform/CudaDriver/CudaDriver.hpp"

#include "Platform/DynamicLibrary/DynamicLibrary.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

namespace singularity::platform::cuda {
namespace {

#if defined(_WIN32)
#define CUDA_DRIVER_API __stdcall
constexpr const char *kDriverLibrary = "nvcuda.dll";
#else
#define CUDA_DRIVER_API
constexpr const char *kDriverLibrary = "libcuda.so.1";
#endif

// The subset of the CUDA driver API this file uses, by the exported symbol
// names cuda.h maps its public names to. These signatures are part of the
// driver's stable binary interface.
using CUresult = int;
using CUdevice = int;
using CUdeviceptr = unsigned long long;
struct CUctx_st;
struct CUmod_st;
struct CUfunc_st;
struct CUevent_st;
using CUcontext = CUctx_st *;
using CUmodule = CUmod_st *;
using CUfunction = CUfunc_st *;
using CUevent = CUevent_st *;
constexpr CUresult kSuccess = 0;
constexpr int kAttributeIntegrated = 18;
constexpr int kAttributeComputeCapabilityMajor = 75;
constexpr int kAttributeComputeCapabilityMinor = 76;

struct DriverApi {
	DynamicLibrary library;
	CUresult(CUDA_DRIVER_API *init)(unsigned int) = nullptr;
	CUresult(CUDA_DRIVER_API *driver_get_version)(int *) = nullptr;
	CUresult(CUDA_DRIVER_API *device_get_count)(int *) = nullptr;
	CUresult(CUDA_DRIVER_API *device_get)(CUdevice *, int) = nullptr;
	CUresult(CUDA_DRIVER_API *device_get_name)(char *, int, CUdevice) = nullptr;
	CUresult(CUDA_DRIVER_API *device_total_mem)(std::size_t *, CUdevice) = nullptr;
	CUresult(CUDA_DRIVER_API *device_get_attribute)(int *, int, CUdevice) = nullptr;
	CUresult(CUDA_DRIVER_API *device_get_pci_bus_id)(char *, int, CUdevice) = nullptr;
	CUresult(CUDA_DRIVER_API *device_get_luid)(char *, unsigned int *, CUdevice) = nullptr;
	CUresult(CUDA_DRIVER_API *primary_ctx_retain)(CUcontext *, CUdevice) = nullptr;
	CUresult(CUDA_DRIVER_API *primary_ctx_release)(CUdevice) = nullptr;
	CUresult(CUDA_DRIVER_API *ctx_set_current)(CUcontext) = nullptr;
	CUresult(CUDA_DRIVER_API *mem_alloc)(CUdeviceptr *, std::size_t) = nullptr;
	CUresult(CUDA_DRIVER_API *mem_free)(CUdeviceptr) = nullptr;
	CUresult(CUDA_DRIVER_API *mem_host_alloc)(void **, std::size_t, unsigned int) = nullptr;
	CUresult(CUDA_DRIVER_API *mem_free_host)(void *) = nullptr;
	CUresult(CUDA_DRIVER_API *memcpy_htod)(CUdeviceptr, const void *, std::size_t) = nullptr;
	CUresult(CUDA_DRIVER_API *memset_d32)(CUdeviceptr, unsigned int, std::size_t) = nullptr;
	CUresult(CUDA_DRIVER_API *module_load_data)(CUmodule *, const void *) = nullptr;
	CUresult(CUDA_DRIVER_API *module_get_function)(CUfunction *, CUmodule, const char *) = nullptr;
	CUresult(CUDA_DRIVER_API *module_unload)(CUmodule) = nullptr;
	CUresult(CUDA_DRIVER_API *launch_kernel)(CUfunction, unsigned int, unsigned int, unsigned int, unsigned int,
		unsigned int, unsigned int, unsigned int, void *, void **, void **) = nullptr;
	CUresult(CUDA_DRIVER_API *event_create)(CUevent *, unsigned int) = nullptr;
	CUresult(CUDA_DRIVER_API *event_record)(CUevent, void *) = nullptr;
	CUresult(CUDA_DRIVER_API *event_synchronize)(CUevent) = nullptr;
	CUresult(CUDA_DRIVER_API *event_elapsed_time)(float *, CUevent, CUevent) = nullptr;
	CUresult(CUDA_DRIVER_API *event_destroy)(CUevent) = nullptr;

	explicit DriverApi(DynamicLibrary opened) : library(std::move(opened)) {
		init = library.function<decltype(init)>("cuInit");
		driver_get_version = library.function<decltype(driver_get_version)>("cuDriverGetVersion");
		device_get_count = library.function<decltype(device_get_count)>("cuDeviceGetCount");
		device_get = library.function<decltype(device_get)>("cuDeviceGet");
		device_get_name = library.function<decltype(device_get_name)>("cuDeviceGetName");
		device_total_mem = library.function<decltype(device_total_mem)>("cuDeviceTotalMem_v2");
		device_get_attribute = library.function<decltype(device_get_attribute)>("cuDeviceGetAttribute");
		device_get_pci_bus_id = library.function<decltype(device_get_pci_bus_id)>("cuDeviceGetPCIBusId");
		device_get_luid = library.function<decltype(device_get_luid)>("cuDeviceGetLuid");
		primary_ctx_retain = library.function<decltype(primary_ctx_retain)>("cuDevicePrimaryCtxRetain");
		primary_ctx_release = library.function<decltype(primary_ctx_release)>("cuDevicePrimaryCtxRelease_v2");
		ctx_set_current = library.function<decltype(ctx_set_current)>("cuCtxSetCurrent");
		mem_alloc = library.function<decltype(mem_alloc)>("cuMemAlloc_v2");
		mem_free = library.function<decltype(mem_free)>("cuMemFree_v2");
		mem_host_alloc = library.function<decltype(mem_host_alloc)>("cuMemHostAlloc");
		mem_free_host = library.function<decltype(mem_free_host)>("cuMemFreeHost");
		memcpy_htod = library.function<decltype(memcpy_htod)>("cuMemcpyHtoD_v2");
		memset_d32 = library.function<decltype(memset_d32)>("cuMemsetD32_v2");
		module_load_data = library.function<decltype(module_load_data)>("cuModuleLoadData");
		module_get_function = library.function<decltype(module_get_function)>("cuModuleGetFunction");
		module_unload = library.function<decltype(module_unload)>("cuModuleUnload");
		launch_kernel = library.function<decltype(launch_kernel)>("cuLaunchKernel");
		event_create = library.function<decltype(event_create)>("cuEventCreate");
		event_record = library.function<decltype(event_record)>("cuEventRecord");
		event_synchronize = library.function<decltype(event_synchronize)>("cuEventSynchronize");
		event_elapsed_time = library.function<decltype(event_elapsed_time)>("cuEventElapsedTime");
		event_destroy = library.function<decltype(event_destroy)>("cuEventDestroy_v2");
	}

	bool can_query() const {
		return init && driver_get_version && device_get_count && device_get && device_get_name &&
			device_total_mem && device_get_attribute;
	}
	bool can_measure() const {
		return primary_ctx_retain && primary_ctx_release && ctx_set_current && mem_alloc && mem_free &&
			mem_host_alloc && mem_free_host && memcpy_htod && memset_d32 && module_load_data &&
			module_get_function && module_unload && launch_kernel && event_create && event_record &&
			event_synchronize && event_elapsed_time && event_destroy;
	}
};

// Loaded once and kept for the life of the process: the driver installs its
// own process-exit handling, so it is never unloaded from under it.
const DriverApi *driver() {
	static const DriverApi *api = [] () -> const DriverApi * {
		auto library = DynamicLibrary::open_system(kDriverLibrary);
		if (!library) return nullptr;
		auto *loaded = new DriverApi(std::move(*library));
		if (!loaded->can_query() || loaded->init(0) != kSuccess) {
			delete loaded;
			return nullptr;
		}
		return loaded;
	}();
	return api;
}

// The calibration kernel, as PTX the driver compiles for whatever device it
// runs on: output[i] = input[i] * 1.000001f + 0.000001f, one element per
// thread - exactly the Metal calibration kernel. Two targets: the oldest
// one first for older drivers, Turing's for drivers that no longer accept
// pre-Turing PTX.
constexpr const char *kCalibrationPtx[] = {
	R"(.version 6.0
.target sm_52
.address_size 64
.visible .entry calibration_kernel(.param .u64 input, .param .u64 output, .param .u32 count)
{
	.reg .pred %p<2>;
	.reg .f32 %f<3>;
	.reg .b32 %r<6>;
	.reg .b64 %rd<8>;
	ld.param.u64 %rd1, [input];
	ld.param.u64 %rd2, [output];
	ld.param.u32 %r2, [count];
	mov.u32 %r3, %ctaid.x;
	mov.u32 %r4, %ntid.x;
	mov.u32 %r5, %tid.x;
	mad.lo.s32 %r1, %r3, %r4, %r5;
	setp.ge.u32 %p1, %r1, %r2;
	@%p1 bra DONE;
	cvta.to.global.u64 %rd3, %rd1;
	mul.wide.u32 %rd4, %r1, 4;
	add.s64 %rd5, %rd3, %rd4;
	ld.global.f32 %f1, [%rd5];
	fma.rn.f32 %f2, %f1, 0f3F800008, 0f358637BD;
	cvta.to.global.u64 %rd6, %rd2;
	add.s64 %rd7, %rd6, %rd4;
	st.global.f32 [%rd7], %f2;
DONE:
	ret;
}
)",
	R"(.version 7.0
.target sm_75
.address_size 64
.visible .entry calibration_kernel(.param .u64 input, .param .u64 output, .param .u32 count)
{
	.reg .pred %p<2>;
	.reg .f32 %f<3>;
	.reg .b32 %r<6>;
	.reg .b64 %rd<8>;
	ld.param.u64 %rd1, [input];
	ld.param.u64 %rd2, [output];
	ld.param.u32 %r2, [count];
	mov.u32 %r3, %ctaid.x;
	mov.u32 %r4, %ntid.x;
	mov.u32 %r5, %tid.x;
	mad.lo.s32 %r1, %r3, %r4, %r5;
	setp.ge.u32 %p1, %r1, %r2;
	@%p1 bra DONE;
	cvta.to.global.u64 %rd3, %rd1;
	mul.wide.u32 %rd4, %r1, 4;
	add.s64 %rd5, %rd3, %rd4;
	ld.global.f32 %f1, [%rd5];
	fma.rn.f32 %f2, %f1, 0f3F800008, 0f358637BD;
	cvta.to.global.u64 %rd6, %rd2;
	add.s64 %rd7, %rd6, %rd4;
	st.global.f32 [%rd7], %f2;
DONE:
	ret;
}
)",
};

} // namespace

std::optional<DriverInfo> query_driver() {
	static std::once_flag once;
	static std::optional<DriverInfo> cached;
	std::call_once(once, [] {
		const DriverApi *api = driver();
		if (api == nullptr) return;
		DriverInfo info;
		int count = 0;
		if (api->driver_get_version(&info.driver_version) != kSuccess ||
			api->device_get_count(&count) != kSuccess || count <= 0) return;
		for (int ordinal = 0; ordinal < count; ++ordinal) {
			CUdevice handle = 0;
			if (api->device_get(&handle, ordinal) != kSuccess) continue;
			Device device;
			device.ordinal = ordinal;
			char name[256] = {};
			if (api->device_get_name(name, static_cast<int>(sizeof(name) - 1), handle) == kSuccess) device.name = name;
			std::size_t total = 0;
			if (api->device_total_mem(&total, handle) == kSuccess) device.total_memory_bytes = total;
			int value = 0;
			if (api->device_get_attribute(&value, kAttributeComputeCapabilityMajor, handle) == kSuccess)
				device.compute_capability_major = value;
			if (api->device_get_attribute(&value, kAttributeComputeCapabilityMinor, handle) == kSuccess)
				device.compute_capability_minor = value;
			if (api->device_get_attribute(&value, kAttributeIntegrated, handle) == kSuccess)
				device.integrated = value != 0;
			char bus[32] = {};
			if (api->device_get_pci_bus_id &&
				api->device_get_pci_bus_id(bus, static_cast<int>(sizeof(bus) - 1), handle) == kSuccess)
				device.pci_bus_id = bus;
#if defined(_WIN32)
			char luid[8] = {};
			unsigned int node_mask = 0;
			if (api->device_get_luid && api->device_get_luid(luid, &node_mask, handle) == kSuccess) {
				std::array<std::uint8_t, 8> bytes{};
				std::memcpy(bytes.data(), luid, bytes.size());
				device.luid = bytes;
			}
#endif
			if (!device.name.empty() && device.total_memory_bytes > 0) info.devices.push_back(std::move(device));
		}
		if (!info.devices.empty()) cached = std::move(info);
	});
	return cached;
}

DeviceMeasurement measure_device(int ordinal) {
	DeviceMeasurement result;
	const DriverApi *api = driver();
	if (api == nullptr) {
		result.unavailable_reason = "No NVIDIA driver is installed.";
		return result;
	}
	if (!api->can_measure()) {
		result.unavailable_reason = "The installed NVIDIA driver does not expose the calibration interface.";
		return result;
	}
	CUdevice device = 0;
	CUcontext context = nullptr;
	if (api->device_get(&device, ordinal) != kSuccess || api->primary_ctx_retain(&context, device) != kSuccess) {
		result.unavailable_reason = "The NVIDIA device could not be opened for calibration.";
		return result;
	}

	constexpr std::uint64_t kElementCount = 16ULL * 1024 * 1024;
	constexpr std::uint64_t kBufferBytes = kElementCount * sizeof(float);
	constexpr int kIterations = 4;
	CUdeviceptr input = 0;
	CUdeviceptr output = 0;
	void *pinned = nullptr;
	CUmodule module = nullptr;
	CUevent started = nullptr;
	CUevent finished = nullptr;
	auto release = [&] {
		if (started) api->event_destroy(started);
		if (finished) api->event_destroy(finished);
		if (module) api->module_unload(module);
		if (pinned) api->mem_free_host(pinned);
		if (output) api->mem_free(output);
		if (input) api->mem_free(input);
		api->ctx_set_current(nullptr);
		api->primary_ctx_release(device);
	};
	if (api->ctx_set_current(context) != kSuccess || api->mem_alloc(&input, kBufferBytes) != kSuccess ||
		api->mem_alloc(&output, kBufferBytes) != kSuccess ||
		api->memset_d32(input, 0x3F800000U /* 1.0f */, kElementCount) != kSuccess ||
		api->event_create(&started, 0) != kSuccess || api->event_create(&finished, 0) != kSuccess) {
		result.unavailable_reason = "The NVIDIA device could not allocate the calibration resources.";
		release();
		return result;
	}
	for (const char *ptx : kCalibrationPtx) {
		if (api->module_load_data(&module, ptx) == kSuccess) break;
		module = nullptr;
	}
	CUfunction kernel = nullptr;
	if (module == nullptr || api->module_get_function(&kernel, module, "calibration_kernel") != kSuccess) {
		result.unavailable_reason = "The NVIDIA driver could not compile the calibration kernel.";
		release();
		return result;
	}

	unsigned int count = static_cast<unsigned int>(kElementCount);
	void *arguments[] = {&input, &output, &count};
	constexpr unsigned int kThreads = 256;
	const unsigned int blocks = static_cast<unsigned int>((kElementCount + kThreads - 1) / kThreads);
	auto launch = [&] {
		return api->launch_kernel(kernel, blocks, 1, 1, kThreads, 1, 1, 0, nullptr, arguments, nullptr) == kSuccess;
	};
	auto elapsed_seconds = [&](double *seconds) {
		float milliseconds = 0.0f;
		if (api->event_synchronize(finished) != kSuccess ||
			api->event_elapsed_time(&milliseconds, started, finished) != kSuccess) return false;
		*seconds = static_cast<double>(milliseconds) / 1000.0;
		return true;
	};

	// Warm-up: JIT, first-touch and clock ramp are excluded from the sample.
	if (!launch() || api->event_record(finished, nullptr) != kSuccess || api->event_synchronize(finished) != kSuccess) {
		result.unavailable_reason = "The NVIDIA calibration kernel could not run.";
		release();
		return result;
	}
	double device_seconds = 0.0;
	double wall_seconds = 0.0;
	for (int iteration = 0; iteration < kIterations; ++iteration) {
		const auto wall_started = std::chrono::steady_clock::now();
		double seconds = 0.0;
		if (api->event_record(started, nullptr) != kSuccess || !launch() ||
			api->event_record(finished, nullptr) != kSuccess || !elapsed_seconds(&seconds)) {
			result.unavailable_reason = "The NVIDIA calibration kernel could not run.";
			release();
			return result;
		}
		wall_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_started).count();
		device_seconds += seconds;
	}
	if (device_seconds <= 0.0) {
		result.unavailable_reason = "The NVIDIA driver did not expose calibration timing.";
		release();
		return result;
	}
	const double operations = static_cast<double>(kElementCount) * 2.0 * kIterations;
	const double transferred = static_cast<double>(kBufferBytes) * 2.0 * kIterations;
	result.execution_giga_operations_per_second = operations / device_seconds / 1'000'000'000.0;
	result.memory_bandwidth_bytes_per_second = static_cast<std::uint64_t>(transferred / device_seconds);
	result.command_overhead_microseconds =
		std::max(0.0, (wall_seconds - device_seconds) / kIterations * 1'000'000.0);

	// Host-to-device rate from pinned (page-locked) memory: the path model
	// weights and offloaded state take across the interconnect.
	if (api->mem_host_alloc(&pinned, kBufferBytes, 0) == kSuccess) {
		std::memset(pinned, 0, static_cast<std::size_t>(kBufferBytes));
		double copy_seconds = 0.0;
		bool copied = api->memcpy_htod(input, pinned, kBufferBytes) == kSuccess; // warm-up
		for (int iteration = 0; copied && iteration < kIterations; ++iteration) {
			double seconds = 0.0;
			copied = api->event_record(started, nullptr) == kSuccess &&
				api->memcpy_htod(input, pinned, kBufferBytes) == kSuccess &&
				api->event_record(finished, nullptr) == kSuccess && elapsed_seconds(&seconds);
			copy_seconds += seconds;
		}
		if (copied && copy_seconds > 0.0)
			result.host_to_device_bytes_per_second =
				static_cast<std::uint64_t>(static_cast<double>(kBufferBytes) * kIterations / copy_seconds);
	}
	release();
	return result;
}

std::optional<std::uint64_t> free_memory_bytes(const Device &device) {
	if (device.pci_bus_id.empty()) return std::nullopt;
#if defined(_WIN32)
	constexpr const char *kManagementLibrary = "nvml.dll";
#else
	constexpr const char *kManagementLibrary = "libnvidia-ml.so.1";
#endif
	struct NvmlMemory {
		unsigned long long total;
		unsigned long long free;
		unsigned long long used;
	};
	struct NvmlDevice;
	// nvmlInit_v2 is reference-counted by the library and balanced with
	// nvmlShutdown, so this query never leaves NVML initialized behind it.
	static const auto library = DynamicLibrary::open_system(kManagementLibrary);
	if (!library) return std::nullopt;
	using Init = int(CUDA_DRIVER_API *)();
	using Shutdown = int(CUDA_DRIVER_API *)();
	using ByPciBusId = int(CUDA_DRIVER_API *)(const char *, NvmlDevice **);
	using MemoryInfo = int(CUDA_DRIVER_API *)(NvmlDevice *, NvmlMemory *);
	const auto init = library->function<Init>("nvmlInit_v2");
	const auto shutdown = library->function<Shutdown>("nvmlShutdown");
	const auto by_pci_bus_id = library->function<ByPciBusId>("nvmlDeviceGetHandleByPciBusId_v2");
	const auto memory_info = library->function<MemoryInfo>("nvmlDeviceGetMemoryInfo");
	if (!init || !shutdown || !by_pci_bus_id || !memory_info || init() != 0) return std::nullopt;
	std::optional<std::uint64_t> free;
	NvmlDevice *handle = nullptr;
	NvmlMemory memory{};
	if (by_pci_bus_id(device.pci_bus_id.c_str(), &handle) == 0 && memory_info(handle, &memory) == 0)
		free = memory.free;
	shutdown();
	return free;
}

} // namespace singularity::platform::cuda
