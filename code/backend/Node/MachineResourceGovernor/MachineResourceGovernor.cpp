#include "Node/MachineResourceGovernor/MachineResourceGovernor.hpp"

#include "LocalModel/ExecutionCompatibility/ExecutionCompatibility.hpp"

#include <algorithm>
#include <unordered_map>

namespace singularity::hardware {

struct MachineResourceGovernor::State {
	mutable std::mutex mutex;
	std::uint64_t next_id = 1;
	std::uint64_t reserved_host_bytes = 0;
	std::uint64_t reserved_accelerator_bytes = 0;
	std::unordered_map<std::uint64_t, MachineResourceRequest> leases;
};

namespace {

std::uint64_t subtract_saturated(std::uint64_t value, std::uint64_t used) {
	return used < value ? value - used : 0;
}

std::uint64_t physical_bytes(const MachineResourceRequest &request) {
	return request.unified_memory
		? std::max(request.host_memory_bytes, request.accelerator_memory_bytes)
		: request.host_memory_bytes;
}

} // namespace

MachineResourceGovernor::Lease::Lease(std::shared_ptr<State> state, std::uint64_t id) noexcept
	: state_(std::move(state)), id_(id) {}

MachineResourceGovernor::Lease::Lease(Lease &&other) noexcept
	: state_(std::move(other.state_)), id_(other.id_) { other.id_ = 0; }

MachineResourceGovernor::Lease &MachineResourceGovernor::Lease::operator=(Lease &&other) noexcept {
	if (this == &other) return *this;
	release();
	state_ = std::move(other.state_);
	id_ = other.id_;
	other.id_ = 0;
	return *this;
}

MachineResourceGovernor::Lease::~Lease() { release(); }

void MachineResourceGovernor::Lease::release() noexcept {
	if (!state_ || id_ == 0) return;
	const std::lock_guard<std::mutex> lock(state_->mutex);
	const auto found = state_->leases.find(id_);
	if (found != state_->leases.end()) {
		const auto &request = found->second;
		state_->reserved_host_bytes -= std::min(state_->reserved_host_bytes, physical_bytes(request));
		if (!request.unified_memory) {
			state_->reserved_accelerator_bytes -= std::min(
				state_->reserved_accelerator_bytes, request.accelerator_memory_bytes);
		}
		state_->leases.erase(found);
	}
	id_ = 0;
	state_.reset();
}

MachineResourceGovernor::MachineResourceGovernor() {
	static const auto machine_state = std::make_shared<State>();
	state_ = machine_state;
}

MachineResourceEnvelope MachineResourceGovernor::available_envelope(
		const HardwareCapabilities &hardware) const {
	return available_envelope(hardware, 0);
}

MachineResourceEnvelope MachineResourceGovernor::available_envelope(
		const HardwareCapabilities &hardware, std::uint64_t excluding_lease_id) const {
	MachineResourceEnvelope result;
	result.memory_pressure = hardware.memory_pressure;
	const std::lock_guard<std::mutex> lock(state_->mutex);
	// Exactly the bytes Lease::release() would return for this one lease -
	// the same accounting, so the excluded view equals the envelope its
	// holder's own reload would observe after releasing it.
	std::uint64_t excluded_host_bytes = 0;
	std::uint64_t excluded_accelerator_bytes = 0;
	if (const auto excluded = state_->leases.find(excluding_lease_id); excluded != state_->leases.end()) {
		const auto &request = excluded->second;
		excluded_host_bytes = physical_bytes(request);
		if (!request.unified_memory) excluded_accelerator_bytes = request.accelerator_memory_bytes;
	}
	const auto reserved_host_bytes = subtract_saturated(state_->reserved_host_bytes, excluded_host_bytes);
	const auto reserved_accelerator_bytes =
		subtract_saturated(state_->reserved_accelerator_bytes, excluded_accelerator_bytes);
	const auto host_budget = runtime_memory_budget_bytes(hardware);
	result.host_memory_bytes = subtract_saturated(host_budget, reserved_host_bytes);
	if (hardware.accelerator_memory_budget_bytes) {
		result.accelerator_memory_bytes = hardware.unified_memory
			? std::min(*hardware.accelerator_memory_budget_bytes, result.host_memory_bytes)
			: subtract_saturated(*hardware.accelerator_memory_budget_bytes, reserved_accelerator_bytes);
	}
	return result;
}

std::optional<MachineResourceGovernor::Lease> MachineResourceGovernor::try_acquire(
		const HardwareCapabilities &hardware,
		const MachineResourceRequest &request) {
	if (hardware.memory_pressure == MemoryPressureLevel::Critical || request.owner.empty())
		return std::nullopt;
	const std::lock_guard<std::mutex> lock(state_->mutex);
	const auto host_budget = runtime_memory_budget_bytes(hardware);
	const auto host_needed = physical_bytes(request);
	if (host_needed > subtract_saturated(host_budget, state_->reserved_host_bytes))
		return std::nullopt;
	if (!request.unified_memory && request.accelerator_memory_bytes > 0) {
		if (!hardware.accelerator_memory_budget_bytes || request.accelerator_memory_bytes >
			subtract_saturated(*hardware.accelerator_memory_budget_bytes,
				state_->reserved_accelerator_bytes)) return std::nullopt;
		// Where the OS backs accelerator allocations with system commit, the
		// host and accelerator bytes together must also fit the commit still
		// available, less the same responsiveness reserve; unknown commit is
		// not enough.
		if (hardware.accelerator_memory_charges_commit) {
			if (!hardware.available_commit_bytes) return std::nullopt;
			const auto commit_budget = subtract_saturated(*hardware.available_commit_bytes,
				audit_runtime_memory_budget(hardware).responsiveness_reserve_bytes);
			const auto commit_reserved = state_->reserved_host_bytes + state_->reserved_accelerator_bytes;
			if (host_needed > subtract_saturated(commit_budget, commit_reserved) ||
				request.accelerator_memory_bytes >
					subtract_saturated(commit_budget, commit_reserved) - host_needed) return std::nullopt;
		}
	}
	const auto id = state_->next_id++;
	state_->leases.emplace(id, request);
	state_->reserved_host_bytes += host_needed;
	if (!request.unified_memory) state_->reserved_accelerator_bytes += request.accelerator_memory_bytes;
	return Lease(state_, id);
}

std::size_t MachineResourceGovernor::active_lease_count() const {
	const std::lock_guard<std::mutex> lock(state_->mutex);
	return state_->leases.size();
}

const char *to_string(WorkloadQos qos) {
	switch (qos) {
		case WorkloadQos::ForegroundInteractive: return "foreground_interactive";
		case WorkloadQos::SynchronousRequired: return "synchronous_required";
		case WorkloadQos::Sustained: return "sustained";
		case WorkloadQos::BackgroundOpportunistic: return "background_opportunistic";
	}
	return "unknown";
}

} // namespace singularity::hardware
