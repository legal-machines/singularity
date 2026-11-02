#include "Platform/AcceleratorCalibration/AcceleratorCalibration.hpp"

#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>

namespace singularity::platform {

AcceleratorCapabilities inspect_accelerator_capabilities() {
	@autoreleasepool {
		AcceleratorCapabilities result;
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (device == nil) return result;
		result.identity = device.name.UTF8String;
		result.unified_memory = device.hasUnifiedMemory;
		result.current_allocated_bytes = static_cast<std::uint64_t>(device.currentAllocatedSize);
		result.recommended_working_set_bytes =
			static_cast<std::uint64_t>(device.recommendedMaxWorkingSetSize);
		if (@available(macOS 13.0, *)) {
			result.maximum_transfer_bytes_per_second = static_cast<std::uint64_t>(device.maxTransferRate);
			result.supports_metal_io =
				[device respondsToSelector:@selector(newIOCommandQueueWithDescriptor:error:)];
		}
		if (@available(macOS 15.0, *)) {
			result.supports_residency_sets =
				[device respondsToSelector:@selector(newResidencySetWithDescriptor:error:)];
		}
		[device release];
		return result;
	}
}

AcceleratorCalibration measure_accelerator_calibration() {
	@autoreleasepool {
		AcceleratorCalibration result;
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (device == nil) {
			result.unavailable_reason = "No Metal device is available.";
			return result;
		}

		NSError *error = nil;
		NSString *source = @"kernel void lm_calibration(device const float* input [[buffer(0)]], device float* output [[buffer(1)]], uint index [[thread_position_in_grid]]) { output[index] = input[index] * 1.000001f + 0.000001f; }";
		id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
		if (library == nil) {
			result.unavailable_reason = "Metal could not compile the calibration kernel.";
			[device release];
			return result;
		}
		id<MTLFunction> function = [library newFunctionWithName:@"lm_calibration"];
		id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
		if (pipeline == nil) {
			result.unavailable_reason = "Metal could not create the calibration pipeline.";
			[function release];
			[library release];
			[device release];
			return result;
		}

		constexpr std::uint64_t elementCount = 16ULL * 1024 * 1024;
		constexpr std::uint64_t bufferBytes = elementCount * sizeof(float);
		id<MTLBuffer> input = [device newBufferWithLength:bufferBytes options:MTLResourceStorageModeShared];
		id<MTLBuffer> output = [device newBufferWithLength:bufferBytes options:MTLResourceStorageModeShared];
		id<MTLCommandQueue> queue = [device newCommandQueue];
		auto release_resources = [&] {
			[queue release];
			[output release];
			[input release];
			[pipeline release];
			[function release];
			[library release];
			[device release];
		};
		if (input == nil || output == nil || queue == nil) {
			result.unavailable_reason = "Metal could not allocate the calibration resources.";
			release_resources();
			return result;
		}

		float *values = static_cast<float*>([input contents]);
		for (std::uint64_t index = 0; index < elementCount; ++index) values[index] = 1.0f;

		auto submit = [&]() -> id<MTLCommandBuffer> {
			id<MTLCommandBuffer> command = [queue commandBuffer];
			id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
			[encoder setComputePipelineState:pipeline];
			[encoder setBuffer:input offset:0 atIndex:0];
			[encoder setBuffer:output offset:0 atIndex:1];
			const NSUInteger width = pipeline.threadExecutionWidth;
			[encoder dispatchThreads:MTLSizeMake(elementCount, 1, 1)
				threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
			[encoder endEncoding];
			[command commit];
			return command;
		};

		id<MTLCommandBuffer> warmup = submit();
		[warmup waitUntilCompleted];

		constexpr int iterations = 4;
		double gpuSeconds = 0.0;
		double wallSeconds = 0.0;
		for (int iteration = 0; iteration < iterations; ++iteration) {
			const auto started = std::chrono::steady_clock::now();
			id<MTLCommandBuffer> command = submit();
			[command waitUntilCompleted];
			wallSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
			if (command.GPUEndTime > command.GPUStartTime) {
				gpuSeconds += command.GPUEndTime - command.GPUStartTime;
			}
		}
		if (gpuSeconds <= 0.0) {
			result.unavailable_reason = "Metal did not expose calibration timing.";
			release_resources();
			return result;
		}

		const double operations = static_cast<double>(elementCount) * 2.0 * iterations;
		const double transferredBytes = static_cast<double>(bufferBytes) * 2.0 * iterations;
		result.execution_giga_operations_per_second = operations / gpuSeconds / 1'000'000'000.0;
		result.memory_bandwidth_bytes_per_second = static_cast<std::uint64_t>(transferredBytes / gpuSeconds);
		result.command_overhead_microseconds =
			std::max(0.0, (wallSeconds - gpuSeconds) / iterations * 1'000'000.0);
		release_resources();
		return result;
	}
}

} // namespace singularity::platform
