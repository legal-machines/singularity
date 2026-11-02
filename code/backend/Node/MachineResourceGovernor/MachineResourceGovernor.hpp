#pragma once

#include "Node/DeviceCapabilities/DeviceCapabilities.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace singularity::hardware {

enum class WorkloadQos {
	ForegroundInteractive,
	SynchronousRequired,
	Sustained,
	BackgroundOpportunistic,
};

struct MachineResourceRequest {
	std::string owner;
	std::uint64_t host_memory_bytes = 0;
	std::uint64_t accelerator_memory_bytes = 0;
	bool unified_memory = false;
	bool reclaimable = false;
	WorkloadQos qos = WorkloadQos::ForegroundInteractive;
};

struct MachineResourceEnvelope {
	std::uint64_t host_memory_bytes = 0;
	std::optional<std::uint64_t> accelerator_memory_bytes;
	MemoryPressureLevel memory_pressure = MemoryPressureLevel::Unknown;
};

class MachineResourceGovernor final {
private:
	struct State;

public:
	class Lease final {
	public:
		Lease() = default;
		Lease(const Lease &) = delete;
		Lease &operator=(const Lease &) = delete;
		Lease(Lease &&other) noexcept;
		Lease &operator=(Lease &&other) noexcept;
		~Lease();
		explicit operator bool() const noexcept { return id_ != 0; }
		std::uint64_t id() const noexcept { return id_; }

	private:
		friend class MachineResourceGovernor;
		Lease(std::shared_ptr<State> state, std::uint64_t id) noexcept;
		void release() noexcept;
		std::shared_ptr<State> state_;
		std::uint64_t id_ = 0;
	};

	MachineResourceGovernor();
	MachineResourceEnvelope available_envelope(const HardwareCapabilities &hardware) const;
	// The envelope as it would be if the ONE lease with id `excluding_lease_id`
	// were released first - never every lease under an owner name (two
	// registry entries for the same artifact bytes, or two runtimes in one
	// process, legitimately hold separate leases under one owner). A (re)load
	// of a model releases exactly that model's own lease before it plans
	// (GenerativeModelService::load_entry()), so a capability query for a
	// model that holds its lease but is not running (its worker died on its
	// own, or its load is still starting) plans against this same view - its
	// reservation would otherwise be charged against itself. An id that names
	// no live lease (including 0) sees the ordinary envelope.
	MachineResourceEnvelope available_envelope(const HardwareCapabilities &hardware,
		std::uint64_t excluding_lease_id) const;
	std::optional<Lease> try_acquire(
		const HardwareCapabilities &hardware,
		const MachineResourceRequest &request);
	std::size_t active_lease_count() const;

private:
	std::shared_ptr<State> state_;
};

const char *to_string(WorkloadQos qos);

} // namespace singularity::hardware
