#include "LocalInference/GenerativeModelService/GenerativeModelService.hpp"

#include "LocalInference/PlanEvaluation/PlanEvaluation.hpp"
#include "LocalInference/WorkloadProfile/WorkloadProfile.hpp"
#include "Node/DeviceCapabilities/DeviceCapabilities.hpp"
#include "Node/HardwareCalibration/HardwareCalibration.hpp"
#include "Node/HardwareExecutionGraph/HardwareExecutionGraph.hpp"
#include "Platform/AppPaths/AppPaths.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace singularity::local_inference {
namespace {

using Json = nlohmann::json;

constexpr const char *kUnavailable = "This local model is not available on this computer.";

template <typename T>
Json optional_json(const std::optional<T> &value) {
	return value ? Json(*value) : Json(nullptr);
}

Json runtime_telemetry_json(const RuntimeResourceTelemetry &value, const AdaptiveExecutionPlan &plan,
		const hardware::HardwareCapabilities &machine, bool governor_admitted) {
	Json route = nullptr;
	if (!value.route_trace.empty()) {
		const auto &last = value.route_trace.back();
		route = Json{{"decisionSequence", last.decision_sequence}, {"token", last.token},
			{"layer", last.layer}, {"topK", last.top_k},
			{"exactRoutedIds", last.exact_routed_ids},
			{"hardResidentBefore", last.hard_resident_before},
			{"softResidentEstimateBefore", optional_json(last.soft_resident_estimate_before)},
			{"decisionTimestampNanoseconds", last.decision_timestamp_nanoseconds},
			{"stateReadyTimestampNanoseconds", last.state_ready_timestamp_nanoseconds},
			{"executionTimestampNanoseconds", last.execution_timestamp_nanoseconds}};
	}
	return Json{
		{"schemaVersion", 1},
		{"machine", {
			{"availableMemoryBytes", optional_json(machine.available_system_memory_bytes)},
			{"memoryPressure", hardware::to_string(machine.memory_pressure)},
			{"acceleratorIdentity", machine.accelerator_identity.empty()
				? Json(nullptr) : Json(machine.accelerator_identity)},
			{"acceleratorCurrentAllocatedBytes", optional_json(machine.accelerator_current_allocated_bytes)},
			{"acceleratorRecommendedWorkingSetBytes", optional_json(machine.accelerator_recommended_working_set_bytes)}}},
		{"runtime", {
			{"backend", plan.backend ? Json(hardware::to_string(*plan.backend)) : Json(nullptr)},
			{"strategy", plan.strategy ? Json(to_string(*plan.strategy)) : Json(nullptr)},
			{"feasibility", to_string(plan.feasibility)},
			{"planIdentity", plan.plan_identity.empty() ? Json(nullptr) : Json(plan.plan_identity)},
			{"hostMemoryBudgetBytes", plan.host_memory_budget_bytes},
			{"acceleratorMemoryBudgetBytes", optional_json(plan.accelerator_memory_budget_bytes)},
			{"acceleratorBlockCount", optional_json(plan.accelerator_block_count)},
			{"acceleratorHead", plan.accelerator_head},
			{"hostExpertBlockCount", optional_json(plan.host_expert_block_count)},
			{"governorAdmitted", governor_admitted},
			{"concurrentCpuAccelerator", plan.concurrent_cpu_accelerator}}},
		{"resources", {
			{"currentResidentMemoryBytes", optional_json(value.current_resident_memory_bytes)},
			{"lifetimePeakMemoryBytes", optional_json(value.lifetime_peak_memory_bytes)},
			{"metalCurrentAllocatedBytes", optional_json(value.metal_current_allocated_bytes)},
			{"metalRecommendedWorkingSetBytes", optional_json(value.metal_recommended_working_set_bytes)},
			{"expertCacheCapacity", optional_json(value.expert_cache_capacity)},
			{"expertActiveSlotBytes", optional_json(value.expert_active_slot_bytes)},
			{"expertCacheHits", optional_json(value.expert_cache_hits)},
			{"expertCacheMisses", optional_json(value.expert_cache_misses)},
			{"expertEvictions", optional_json(value.expert_evictions)},
			{"expertBytesLoaded", optional_json(value.expert_bytes_loaded)},
			{"expertAdmissionBytes", optional_json(value.expert_admission_bytes)},
			{"expertStorageLoadNanoseconds", optional_json(value.expert_storage_load_nanoseconds)},
			{"routedExpertRows", optional_json(value.routed_expert_rows)},
			{"cpuExecutedExpertRows", optional_json(value.cpu_executed_expert_rows)},
			{"gpuResidentExpertRows", optional_json(value.gpu_resident_expert_rows)},
			{"gpuAdmittedExpertRows", optional_json(value.gpu_admitted_expert_rows)},
			{"routingAccountingViolations", optional_json(value.routing_accounting_violations)},
			{"prefillCacheHits", optional_json(value.prefill_expert_cache_hits)},
			{"prefillCacheMisses", optional_json(value.prefill_expert_cache_misses)},
			{"prefillExpertBytesLoaded", optional_json(value.prefill_expert_bytes_loaded)},
			{"decodeCacheHits", optional_json(value.decode_expert_cache_hits)},
			{"decodeCacheMisses", optional_json(value.decode_expert_cache_misses)},
			{"decodeExpertBytesLoaded", optional_json(value.decode_expert_bytes_loaded)},
			{"cpuBranchNanoseconds", optional_json(value.cpu_branch_nanoseconds)},
			{"metalBranchWallNanoseconds", optional_json(value.metal_branch_wall_nanoseconds)},
			{"actualSimultaneousExecutionNanoseconds", optional_json(value.actual_simultaneous_execution_nanoseconds)},
			{"branchCriticalPathNanoseconds", optional_json(value.branch_critical_path_nanoseconds)},
			{"processPageIns", optional_json(value.process_page_ins)},
			{"processDiskReadBytes", optional_json(value.process_disk_read_bytes)}}},
		{"physical", {
			{"logicalBytesRequested", optional_json(value.logical_bytes_requested)},
			{"apiReadBytes", optional_json(value.api_read_bytes)},
			{"physicalDeviceBytes", nullptr},
			{"hardOwnedResidentBytes", nullptr},
			{"fileBackedSoftResidentBytes", nullptr},
			{"metalCommittedBytes", nullptr},
			{"peakTransientInFlightBytes", optional_json(value.peak_transient_in_flight_bytes)},
			{"swapBytes", nullptr}, {"compressedBytes", nullptr}}},
		{"routing", {{"traceEntries", value.route_trace.size()},
			{"traceOverflow", value.route_trace_overflow}, {"lastDecision", route}}}
	};
}

class LoadCancelled final : public std::exception {};

// Scans `model_search_path` itself, recursively, for installed *.gguf files -
// exactly the folder the caller named, with no implicit subfolder and no
// environment-variable override.
std::vector<std::filesystem::path> model_artifacts(const std::filesystem::path &model_search_path) {
	std::set<std::filesystem::path> paths;
	std::error_code error;
	if (std::filesystem::is_directory(model_search_path, error) && !error) {
		for (std::filesystem::recursive_directory_iterator iterator(
			model_search_path, std::filesystem::directory_options::skip_permission_denied, error), end;
			iterator != end && !error; iterator.increment(error)) {
			if (iterator->is_regular_file(error) && !error && iterator->path().extension() == ".gguf") {
				paths.insert(iterator->path());
			}
		}
	}
	return {paths.begin(), paths.end()};
}

std::string machine_runtime_identity(const hardware::HardwareCapabilities &hardware) {
	std::ostringstream identity;
	identity << hardware.hardware_model << '|' << hardware.cpu_identity << '|'
		<< hardware.accelerator_identity << '|' << hardware.operating_system_identity;
	for (const auto &runtime : hardware.runtime_identities) identity << '|' << runtime;
	return identity.str();
}

std::uint64_t owned_memory_bytes(const AdaptiveExecutionPlan &plan) {
	std::uint64_t total = 0;
	for (const auto &consumer : plan.memory_consumers) {
		if (consumer.bytes > std::numeric_limits<std::uint64_t>::max() - total)
			return std::numeric_limits<std::uint64_t>::max();
		total += consumer.bytes;
	}
	return total;
}

// A candidate that overrides expert_cache_capacity/expert_cache_bytes on a
// cloned plan (the "compact" and "wide" MoE candidates below) must keep its
// own memory_consumers in sync, or the governor keeps accounting the
// ORIGINAL plan's cache size - under-reserving for a wider cache, or
// over-reserving for a smaller one.
void resize_expert_residency_consumer(AdaptiveExecutionPlan &plan, std::uint64_t new_bytes) {
	for (auto &consumer : plan.memory_consumers) {
		if (consumer.kind == AdaptiveMemoryConsumerKind::ExpertResidency) {
			consumer.bytes = new_bytes;
			return;
		}
	}
}

hardware::HardwareCapabilities capabilities_for_execution_graph(
		const hardware::HardwareCapabilities &detected,
		const hardware::HardwareExecutionGraph &graph) {
	auto planning = detected;
	for (const auto &node : graph.nodes) {
		if (node.kind == hardware::ExecutionNodeKind::Cpu) {
			if (node.available_bytes) planning.available_system_memory_bytes = node.available_bytes;
			continue;
		}
		if (node.kind != hardware::ExecutionNodeKind::Accelerator || !node.safe_working_set_bytes) continue;
		if (!planning.accelerator_memory_budget_bytes) {
			planning.accelerator_memory_budget_bytes = node.safe_working_set_bytes;
		} else {
			planning.accelerator_memory_budget_bytes = std::min(
				*planning.accelerator_memory_budget_bytes, *node.safe_working_set_bytes);
		}
	}
	if (graph.memory_topology == hardware::MemoryTopology::Unified &&
		planning.accelerator_memory_budget_bytes) {
		planning.accelerator_memory_budget_bytes = std::min(
			*planning.accelerator_memory_budget_bytes, hardware::runtime_memory_budget_bytes(planning));
	}
	return planning;
}

hardware::ModelArchitectureKind architecture_kind(ModelArchitecture architecture) {
	switch (architecture) {
		case ModelArchitecture::Dense: return hardware::ModelArchitectureKind::DenseTransformer;
		case ModelArchitecture::MixtureOfExperts: return hardware::ModelArchitectureKind::MixtureOfExperts;
		case ModelArchitecture::Unknown: break;
	}
	return hardware::ModelArchitectureKind::Unknown;
}

// What the Model Library shows for a local generative model. Its compatibility
// is judged by memory alone against the always-available CPU path: which
// accelerator a load really uses is the planner's decision at load time
// (reported in the live state's execution plan), never a promise made here.
hardware::LocalModelDescriptor descriptor_for(const std::string &id,
		const std::filesystem::path &install_path, const ModelExecutionProfile &profile) {
	hardware::LocalModelDescriptor descriptor;
	descriptor.id = id;
	descriptor.display_name = profile.display_name.empty() ? "Local generative model" : profile.display_name;
	descriptor.purpose = "Private local text generation";
	descriptor.role = "Generation";
	descriptor.family = "Local";
	descriptor.developer = "Legal Machines";
	descriptor.version = "GGUF";
	descriptor.install_path = install_path;
	descriptor.preferred_backends = {hardware::ExecutionBackend::Cpu};
	descriptor.resource_requirement = resource_requirement_for(profile);
	descriptor.execution_profile.architecture_kind = architecture_kind(profile.architecture);
	descriptor.execution_profile.architecture_description =
		hardware::to_string(descriptor.execution_profile.architecture_kind);
	descriptor.execution_profile.precision = profile.precision;
	descriptor.execution_profile.quantization =
		profile.quantization && !profile.quantization->empty() ? profile.quantization : std::nullopt;
	descriptor.execution_profile.model_state_bytes = profile.model_state_bytes > 0
		? std::optional<std::uint64_t>(profile.model_state_bytes) : std::nullopt;
	// The package's own advertised maximum: informational (a Library card),
	// never an executable budget - see LocalModelLiveState's planned and
	// effective figures for what budget code uses.
	descriptor.execution_profile.native_context_length =
		profile.attention.context_length && *profile.attention.context_length > 0
			? profile.attention.context_length : std::nullopt;
	// Every generative local model is an autoregressive transformer with real
	// KV-cache state.
	descriptor.execution_profile.requires_runtime_state = true;
	descriptor.required = false;
	return descriptor;
}

} // namespace

std::string stable_artifact_id(const std::filesystem::path &path, const ModelExecutionProfile &profile) {
	std::uint64_t hash = 1469598103934665603ULL;
	const std::string identity = platform::path_to_utf8(std::filesystem::weakly_canonical(path)) + ":" +
		profile.architecture_id + ":" + std::to_string(profile.model_state_bytes);
	for (const unsigned char value : identity) {
		hash ^= value;
		hash *= 1099511628211ULL;
	}
	std::ostringstream output;
	output << "local-gguf-" << std::hex << std::setw(16) << std::setfill('0') << hash;
	return output.str();
}

hardware::MachineResourceRequest resource_request_for(const std::string &owner,
		const AdaptiveExecutionPlan &plan, bool unified_memory) {
	hardware::MachineResourceRequest request;
	request.owner = owner;
	request.unified_memory = unified_memory;
	request.reclaimable = true;
	request.qos = hardware::WorkloadQos::ForegroundInteractive;
	if (unified_memory) {
		// One physical pool: everything the plan owns is host memory, of which
		// the accelerator may address up to its own budget.
		request.host_memory_bytes = owned_memory_bytes(plan);
		request.accelerator_memory_bytes = plan.accelerator_memory_budget_bytes
			? std::min(request.host_memory_bytes, *plan.accelerator_memory_budget_bytes) : 0;
		return request;
	}
	// Two pools: each is charged only what the plan places in it.
	const auto footprint = memory_footprint(plan);
	request.host_memory_bytes = footprint.host_bytes;
	request.accelerator_memory_bytes = footprint.accelerator_bytes;
	return request;
}

hardware::ResourceRequirement resource_requirement_for(const ModelExecutionProfile &profile) {
	hardware::ResourceRequirement requirement;
	// model_state_bytes == 0 means genuinely unknown (inspection failed and
	// even the file size could not be read) - reported honestly as an unknown
	// requirement rather than a bare-headroom figure.
	if (profile.model_state_bytes == 0) return requirement;
	requirement.min_ram_bytes = profile.model_state_bytes + 256ULL * 1024 * 1024;
	requirement.recommended_ram_bytes = requirement.min_ram_bytes;
	requirement.min_accelerator_memory_bytes = profile.model_state_bytes;
	if (profile.architecture == ModelArchitecture::MixtureOfExperts &&
		profile.supports_dynamic_expert_residency && profile.experts.exact_tensor_layout &&
		profile.experts.non_expert_state_bytes && profile.experts.active_experts_per_token &&
		profile.experts.estimated_bytes_per_expert) {
		const std::uint64_t compact_bytes = *profile.experts.non_expert_state_bytes +
			*profile.experts.active_experts_per_token * *profile.experts.estimated_bytes_per_expert +
			256ULL * 1024 * 1024;
		requirement.min_ram_bytes = compact_bytes;
		requirement.min_accelerator_memory_bytes = compact_bytes;
		// Full residency remains the recommendation when it safely fits; the
		// compact requirement is the truthful minimum for reduced performance.
		requirement.recommended_ram_bytes = profile.model_state_bytes + 256ULL * 1024 * 1024;
	}
	return requirement;
}

std::uint64_t resolve_requested_context_tokens(
		const ModelExecutionProfile &profile,
		const hardware::HardwareCapabilities &planning_capabilities,
		const hardware::HardwareCalibration &calibration) {
	constexpr std::uint64_t kSafeDefaultContextTokens = 4096;
	constexpr std::uint64_t kMaximumConsideredContextTokens = 131072;
	if (!profile.attention.context_length || *profile.attention.context_length == 0)
		return kSafeDefaultContextTokens;
	std::uint64_t candidate = std::min(*profile.attention.context_length, kMaximumConsideredContextTokens);
	while (candidate > kSafeDefaultContextTokens) {
		const auto probe = plan_model_execution(profile, planning_capabilities, calibration, candidate);
		if (probe.feasibility == ExecutionFeasibility::Recommended ||
				probe.feasibility == ExecutionFeasibility::ReducedPerformance)
			return candidate;
		candidate /= 2;
	}
	return kSafeDefaultContextTokens;
}

GenerativeModelService::GenerativeModelService(std::filesystem::path model_search_path,
		std::filesystem::path application_data_path, RuntimeFactory runtime_factory)
	: model_search_path_(std::move(model_search_path)),
	  application_data_path_(std::move(application_data_path)),
	  runtime_factory_(std::move(runtime_factory)) {
	if (!runtime_factory_) {
		runtime_factory_ = [logs = application_data_path_ / "Logs"] {
			return std::make_shared<LlamaCppRuntimeAdapter>(logs);
		};
	}
}

GenerativeModelService::~GenerativeModelService() { reset(); }

void GenerativeModelService::discover_and_register(hardware::LocalModelRegistry &registry,
		const std::map<std::string, std::filesystem::path> &managed_install_paths,
		std::function<bool(const std::string &)> unload_model) {
	for (const auto &path : model_artifacts(model_search_path_)) {
		ModelExecutionProfile profile;
		try {
			profile = inspect_model(path);
		} catch (const std::exception &error) {
			profile.display_name = platform::path_to_utf8(path.stem());
			std::error_code size_error;
			profile.model_state_bytes = std::filesystem::file_size(path, size_error);
			if (size_error) profile.model_state_bytes = 0;
			profile.support_reason = "The model metadata could not be read safely: " + std::string(error.what());
		}
		const std::string artifact_id = stable_artifact_id(path, profile);
		// A file the caller already knows - a package it installed, a file the
		// person chose - keeps that id and its own path. Any other model the
		// folder holds is known by its artifact id; its install path names no
		// file, so the Library neither sizes nor removes a model Singularity
		// did not put there.
		std::string id = artifact_id;
		std::filesystem::path install_path = model_search_path_ / ".unmanaged" / artifact_id;
		for (const auto &[managed_id, managed_path] : managed_install_paths) {
			std::error_code error;
			if (std::filesystem::equivalent(path, managed_path, error) && !error) {
				id = managed_id;
				install_path = managed_path;
				break;
			}
		}
		auto entry = std::make_shared<Entry>();
		entry->path = path;
		entry->artifact_id = artifact_id;
		entry->profile = profile;
		entry->runtime = runtime_factory_();
		{
			const std::lock_guard<std::mutex> lock(mutex_);
			// Discovery is an idempotent rescan: a model already known keeps
			// its entry and its registration.
			if (entries_.find(id) != entries_.end()) continue;
			entries_[id] = entry;
		}
		registry.upsert_model(descriptor_for(id, install_path, profile),
			hardware::LocalModelRegistry::HardwareAwareLiveStateProvider(
				[this, id](const hardware::HardwareCapabilities &capabilities) { return live_state(id, capabilities); }),
			[unload_model, id] { return unload_model ? unload_model(id) : true; });
	}
}

void GenerativeModelService::reset() noexcept {
	std::unordered_map<std::string, std::shared_ptr<Entry>> entries;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		entries.swap(entries_);
		for (auto &entry : entries) entry.second->cancel_requested.store(true);
	}
	for (auto &[id, entry] : entries) {
		(void)id;
		const std::lock_guard<std::mutex> runtime_lock(entry->runtime_mutex);
		entry->runtime->stop();
		replace_resource_lease(*entry, std::nullopt);
	}
}

bool GenerativeModelService::contains(const std::string &model_id) const {
	const std::lock_guard<std::mutex> lock(mutex_);
	return entries_.find(model_id) != entries_.end();
}

std::vector<std::string> GenerativeModelService::model_ids() const {
	const std::lock_guard<std::mutex> lock(mutex_);
	std::vector<std::string> ids;
	ids.reserve(entries_.size());
	for (const auto &entry : entries_) ids.push_back(entry.first);
	std::sort(ids.begin(), ids.end());
	return ids;
}

std::optional<std::string> GenerativeModelService::artifact_id_for(const std::string &model_id) const {
	const std::lock_guard<std::mutex> lock(mutex_);
	const auto found = entries_.find(model_id);
	if (found == entries_.end()) return std::nullopt;
	return found->second->artifact_id;
}

std::shared_ptr<GenerativeModelService::Entry> GenerativeModelService::entry_for(const std::string &model_id) const {
	const std::lock_guard<std::mutex> lock(mutex_);
	const auto found = entries_.find(model_id);
	if (found == entries_.end()) throw std::runtime_error(kUnavailable);
	return found->second;
}

void GenerativeModelService::load(const std::string &model_id) {
	const auto entry = entry_for(model_id);
	const std::lock_guard<std::mutex> runtime_lock(entry->runtime_mutex);
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		if (entry->state == hardware::LocalModelLoadState::Ready && entry->runtime->running()) return;
	}
	load_entry(entry);
}

void GenerativeModelService::load_entry(const std::shared_ptr<Entry> &entry) {
	entry->cancel_requested.store(false);
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		entry->state = hardware::LocalModelLoadState::Loading;
		entry->error.clear();
	}
	try {
		// Released before planning: a (re)load competes for the machine
		// envelope without its own previous reservation. planned_context_
		// tokens() plans against this identical view by excluding this entry's
		// own lease (by id) instead.
		replace_resource_lease(*entry, std::nullopt);
		// One hardware snapshot for this whole load: the plan, the cache key,
		// the lease request and the admission check all see the same machine.
		const auto inputs = *resolve_planning_inputs(hardware::detect_hardware_capabilities(),
			/*allow_calibration_measurement=*/true);
		const auto &capabilities = inputs.capabilities;
		const auto profile_root = application_data_path_ / "HardwareProfile";
		const auto &planning_capabilities = inputs.planning_capabilities;
		const auto &calibration = inputs.calibration;
		// Resolved once, from this model's own inspected capability and the
		// machine's real memory envelope: every candidate plan below shares
		// this value and can never disagree about the context it requests.
		const std::uint64_t requested_context_tokens =
			resolve_requested_context_tokens(entry->profile, planning_capabilities, calibration);
		auto plan = plan_model_execution(entry->profile, planning_capabilities, calibration, requested_context_tokens);
		// A realistic interactive request: a legal operation's turn against a
		// ten-row table serializes to ~2,600 tokens, so 3,072 makes a
		// calibration exercise long-prompt prefill, not just decode.
		const auto workload = derive_workload_profile(WorkloadOperation{
			/*prompt_tokens=*/3072, /*expected_generation_tokens=*/128,
			/*concurrency=*/1, /*user_waiting=*/true, /*reusable_prefix_available=*/false,
			/*model_already_loaded=*/false});
		std::vector<CandidatePlan> candidates;
		candidates.push_back(CandidatePlan{plan, std::nullopt, 0.0});
		if (plan.strategy == ExecutionStrategy::SparseHeterogeneous && plan.cpu_worker_budget) {
			const std::uint32_t maximum = *plan.cpu_worker_budget;
			const std::uint32_t middle = std::max(1U, maximum / 2);
			for (const std::uint32_t workers : {1U, middle, maximum}) {
				if (workers == maximum) continue;
				auto worker_candidate = plan;
				worker_candidate.cpu_worker_budget = workers;
				worker_candidate.plan_identity = std::string(to_string(*plan.strategy)) +
					":slots=" + std::to_string(plan.expert_cache_capacity.value_or(0)) +
					":cpu-workers=" + std::to_string(workers) + ":split=adaptive";
				candidates.push_back(CandidatePlan{worker_candidate, std::nullopt,
					static_cast<double>(maximum - workers + 1)});
			}
			for (const double cpu_fraction : {0.25, 0.5, 0.75}) {
				for (const bool concurrent : {true, false}) {
					// Serialized evidence is useful only at the balanced split:
					// the candidate frontier stays bounded.
					if (!concurrent && cpu_fraction != 0.5) continue;
					auto fixed_candidate = plan;
					fixed_candidate.cpu_worker_budget = middle;
					fixed_candidate.target_cpu_work_fraction = cpu_fraction;
					fixed_candidate.concurrent_cpu_accelerator = concurrent;
					fixed_candidate.plan_identity = std::string(to_string(*plan.strategy)) +
						":slots=" + std::to_string(plan.expert_cache_capacity.value_or(0)) +
						":cpu-workers=" + std::to_string(middle) + ":cpu-fraction=" +
						std::to_string(cpu_fraction) + (concurrent ? ":concurrent" : ":serialized");
					candidates.push_back(CandidatePlan{fixed_candidate, std::nullopt, 2.0});
				}
			}
		}
		const auto cpu = plan_model_execution(entry->profile, planning_capabilities, calibration,
			requested_context_tokens, hardware::ExecutionBackend::Cpu);
		if (cpu.strategy) candidates.push_back(CandidatePlan{cpu, std::nullopt, 10.0});
		// The compact/wide expert-cache candidates configure the runtime's
		// dynamic expert residency (its Metal offloader); a runtime without
		// it - a CUDA or CPU build - is never offered one.
		if (entry->profile.architecture == ModelArchitecture::MixtureOfExperts &&
			entry->profile.supports_dynamic_expert_residency &&
			(plan.strategy == ExecutionStrategy::DenseAccelerator ||
			 plan.strategy == ExecutionStrategy::SparseExpertResidency ||
			 plan.strategy == ExecutionStrategy::SparseHeterogeneous) &&
			entry->profile.experts.active_experts_per_token && entry->profile.experts.estimated_bytes_per_expert) {
			const auto active = *entry->profile.experts.active_experts_per_token;
			const auto per_expert = *entry->profile.experts.estimated_bytes_per_expert;
			auto compact = plan;
			compact.strategy = ExecutionStrategy::SparseExpertResidency;
			compact.expert_cache_capacity = active;
			compact.minimum_expert_cache_capacity = active;
			compact.expert_cache_bytes = active * per_expert;
			resize_expert_residency_consumer(compact, *compact.expert_cache_bytes);
			compact.prompt_micro_batch_tokens = 1; // capacity == active: no room for more than one token per microbatch.
			compact.plan_identity = std::string(to_string(*compact.strategy)) + ":slots=" +
				std::to_string(*compact.expert_cache_capacity) + ":ubatch=1";
			compact.feasibility = ExecutionFeasibility::ReducedPerformance;
			if (compact.plan_identity != plan.plan_identity)
				candidates.push_back(CandidatePlan{compact, std::nullopt, 5.0});
			// The file-backed SparseExpertResidency default starts at a
			// conservative active*2 cache, but a long legal-document prompt
			// routes to most of a layer's experts, so that default thrashes
			// the cache almost every microbatch. Measured end to end on one
			// machine (Qwen3-30B-A3B, a real ~2,600-token prompt), the widest
			// cache this plan's own memory and pressure budget proved safe ran
			// 1.77x faster than the conservative default, with cache misses
			// ~74% lower and memory pressure Normal throughout. The widest
			// proven-safe cache - never a hand-picked constant, never above
			// maximum_safe_expert_cache_capacity - is therefore the predicted
			// default; a real measurement still has the final say.
			if (plan.strategy == ExecutionStrategy::SparseExpertResidency &&
				plan.maximum_safe_expert_cache_capacity &&
				*plan.maximum_safe_expert_cache_capacity > plan.expert_cache_capacity.value_or(0)) {
				auto wide = plan;
				wide.expert_cache_capacity = *plan.maximum_safe_expert_cache_capacity;
				wide.expert_cache_bytes = *wide.expert_cache_capacity * per_expert;
				resize_expert_residency_consumer(wide, *wide.expert_cache_bytes);
				wide.prompt_micro_batch_tokens = std::max<std::uint64_t>(1, *wide.expert_cache_capacity / active);
				wide.plan_identity = std::string(to_string(*wide.strategy)) + ":slots=" +
					std::to_string(*wide.expert_cache_capacity) + ":ubatch=" +
					std::to_string(*wide.prompt_micro_batch_tokens);
				candidates.push_back(CandidatePlan{wide, std::nullopt, -1.0});
			}
		}
		const auto cache_path = profile_root / "plan-measurements.json";
		const auto cache_key = plan_measurement_cache_key(machine_runtime_identity(capabilities),
			entry->profile, "local-inference-v3", workload);
		for (auto &candidate : candidates) {
			candidate.cached_measurement = load_plan_measurement(cache_path, cache_key, candidate.plan.plan_identity);
		}
		const auto selection = select_candidate_plan(candidates, workload);
		if (selection.selected_candidate) plan = candidates[*selection.selected_candidate].plan;
		const hardware::MachineResourceRequest resource_request = resource_request_for(
			resource_lease_owner(*entry), plan, capabilities.unified_memory);
		auto lease = resource_governor_.try_acquire(capabilities, resource_request);
		if (!lease) throw std::runtime_error(
			"Another local AI operation is using the safe machine resource envelope. Your Project remains available.");
		entry->runtime->start(entry->path, plan);
		replace_resource_lease(*entry, std::move(*lease));
		if (entry->cancel_requested.exchange(false)) {
			entry->runtime->stop();
			replace_resource_lease(*entry, std::nullopt);
			throw LoadCancelled{};
		}
		const std::lock_guard<std::mutex> lock(mutex_);
		entry->plan = plan;
		entry->state = hardware::LocalModelLoadState::Ready;
	} catch (const LoadCancelled &) {
		replace_resource_lease(*entry, std::nullopt);
		const std::lock_guard<std::mutex> lock(mutex_);
		entry->state = hardware::LocalModelLoadState::Unloaded;
		entry->error.clear();
		throw std::runtime_error("The local model load was cancelled safely.");
	} catch (const std::exception &error) {
		replace_resource_lease(*entry, std::nullopt);
		const std::lock_guard<std::mutex> lock(mutex_);
		entry->state = hardware::LocalModelLoadState::Error;
		entry->error = error.what();
		throw;
	}
}

void GenerativeModelService::unload(const std::string &model_id) noexcept {
	std::shared_ptr<Entry> entry;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		const auto found = entries_.find(model_id);
		if (found == entries_.end()) return;
		entry = found->second;
		entry->cancel_requested.store(true);
	}
	const std::lock_guard<std::mutex> runtime_lock(entry->runtime_mutex);
	entry->runtime->stop();
	replace_resource_lease(*entry, std::nullopt);
	entry->cancel_requested.store(false);
	const std::lock_guard<std::mutex> lock(mutex_);
	entry->state = hardware::LocalModelLoadState::Unloaded;
	entry->error.clear();
}

void GenerativeModelService::prepare_to_serve(const std::shared_ptr<Entry> &entry) {
	if (!entry->runtime->running()) load_entry(entry);
	const auto pressure = hardware::detect_hardware_capabilities().memory_pressure;
	entry->runtime->adapt_to_memory_pressure(pressure);
	if (pressure == hardware::MemoryPressureLevel::Critical || !entry->runtime->running()) {
		replace_resource_lease(*entry, std::nullopt);
		const std::lock_guard<std::mutex> lock(mutex_);
		entry->state = hardware::LocalModelLoadState::Unloaded;
		throw std::runtime_error(
			"Local AI stopped because this computer needs its memory back. Your Project remains safe.");
	}
}

void GenerativeModelService::record_error(Entry &entry, const std::string &error) {
	const std::lock_guard<std::mutex> lock(mutex_);
	entry.error = error;
}

GenerativeResult GenerativeModelService::generate(const std::string &model_id, const GenerativeRequest &request) {
	const auto entry = entry_for(model_id);
	const std::lock_guard<std::mutex> runtime_lock(entry->runtime_mutex);
	prepare_to_serve(entry);
	try {
		auto result = entry->runtime->generate(request);
		const auto resources = entry->runtime->resource_telemetry();
		const auto machine = hardware::detect_hardware_capabilities();
		bool admitted = false;
		{
			const std::lock_guard<std::mutex> lock(mutex_);
			admitted = entry->resource_lease.has_value();
			// A generation that succeeded clears whatever an earlier failed one
			// left: the model is demonstrably serving again.
			entry->error.clear();
		}
		result.telemetry_json = runtime_telemetry_json(resources, entry->plan, machine, admitted).dump();
		return result;
	} catch (const std::exception &error) {
		// A generation a person stopped did not fail.
		if (!(request.stop && request.stop->load())) record_error(*entry, error.what());
		throw;
	}
}

GenerativeRequestMeasurement GenerativeModelService::measure(const std::string &model_id,
		const GenerativeRequest &request) {
	const auto entry = entry_for(model_id);
	const std::lock_guard<std::mutex> runtime_lock(entry->runtime_mutex);
	prepare_to_serve(entry);
	try {
		return entry->runtime->measure(request);
	} catch (const std::exception &error) {
		if (!(request.stop && request.stop->load())) record_error(*entry, error.what());
		throw;
	}
}

std::string GenerativeModelService::resource_lease_owner(const Entry &entry) {
	return entry.profile.artifact_identity.empty() ? platform::path_to_utf8(entry.path) : entry.profile.artifact_identity;
}

void GenerativeModelService::replace_resource_lease(Entry &entry,
		std::optional<hardware::MachineResourceGovernor::Lease> lease) {
	std::optional<hardware::MachineResourceGovernor::Lease> previous;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		previous = std::move(entry.resource_lease);
		entry.resource_lease = std::move(lease);
	}
	// `previous` releases here, outside mutex_.
}

std::optional<GenerativeModelService::PlanningInputs> GenerativeModelService::resolve_planning_inputs(
		const hardware::HardwareCapabilities &capabilities, bool allow_calibration_measurement,
		std::uint64_t excluding_lease_id) const {
	const auto profile_root = application_data_path_ / "HardwareProfile";
	std::optional<hardware::HardwareCalibration> calibration = allow_calibration_measurement
		? std::optional<hardware::HardwareCalibration>(hardware::cached_hardware_calibration(
			profile_root / "calibration.json", profile_root, capabilities))
		: hardware::read_cached_hardware_calibration(profile_root / "calibration.json", capabilities);
	if (!calibration) return std::nullopt;
	const auto execution_graph = hardware::build_hardware_execution_graph(capabilities, *calibration);
	auto planning_capabilities = capabilities_for_execution_graph(capabilities, execution_graph);
	const auto envelope = resource_governor_.available_envelope(capabilities, excluding_lease_id);
	planning_capabilities.available_system_memory_bytes = envelope.host_memory_bytes;
	// The governor's envelope already includes the OS/user reserve: total 0
	// keeps runtime_memory_budget_bytes() from applying it a second time.
	planning_capabilities.total_system_memory_bytes = 0;
	if (envelope.accelerator_memory_bytes)
		planning_capabilities.accelerator_memory_budget_bytes = envelope.accelerator_memory_bytes;
	return PlanningInputs{capabilities, planning_capabilities, *calibration};
}

std::optional<std::uint64_t> GenerativeModelService::planned_context_tokens(const std::string &model_id) const {
	return planned_context_tokens(model_id, hardware::detect_hardware_capabilities());
}

std::optional<std::uint64_t> GenerativeModelService::planned_context_tokens(const std::string &model_id,
		const hardware::HardwareCapabilities &capabilities) const {
	std::shared_ptr<const Entry> entry;
	std::uint64_t own_lease_id = 0;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		const auto found = entries_.find(model_id);
		if (found == entries_.end()) return std::nullopt;
		entry = found->second;
		// Loaded with a live runtime: the plan its current load was made with.
		// Planned again now, it would be charged for the memory its own runtime
		// occupies (a live run on a 24 GiB machine planned 4096 tokens for a
		// model holding a 40960-token grant). Planned and granted agree.
		if (entry->state == hardware::LocalModelLoadState::Ready && entry->plan.backend && entry->runtime->running())
			return entry->plan.requested_context_tokens;
		if (found->second->resource_lease) own_lease_id = found->second->resource_lease->id();
	}
	// entry->profile is written once, before the entry is published, and never
	// again: read in place (a MoE profile's per-expert tensor map is large).
	// Planned exactly as load_entry() would plan a (re)load: a lease it still
	// holds while not running is not part of the envelope it competes for.
	const auto inputs = resolve_planning_inputs(capabilities, /*allow_calibration_measurement=*/false, own_lease_id);
	if (!inputs) return std::nullopt;
	return resolve_requested_context_tokens(entry->profile, inputs->planning_capabilities, inputs->calibration);
}

hardware::LocalModelLiveState GenerativeModelService::live_state(const std::string &model_id,
		const hardware::HardwareCapabilities &capabilities) const {
	hardware::LocalModelLiveState state;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		const auto found = entries_.find(model_id);
		if (found == entries_.end()) return state;
		const Entry &entry = *found->second;
		// Ready means a LIVE runtime: one whose process died on its own (out of
		// memory, an assertion) is not loaded whatever the entry last said, so
		// a caller budgets from the plan and reloads instead of trusting a dead
		// grant. A model that is not serving shows why its last load or
		// generation failed, until one succeeds.
		const bool serving = entry.state == hardware::LocalModelLoadState::Ready && entry.runtime->running();
		if (entry.state == hardware::LocalModelLoadState::Loading) {
			state.load_state = hardware::LocalModelLoadState::Loading;
		} else if (serving) {
			state.load_state = hardware::LocalModelLoadState::Ready;
		} else if (!entry.error.empty()) {
			state.load_state = hardware::LocalModelLoadState::Error;
			state.load_error = entry.error;
		} else {
			state.load_state = hardware::LocalModelLoadState::Unloaded;
		}
		if (entry.plan.backend) {
			state.execution_plan = hardware::ExecutionPlan{*entry.plan.backend, entry.plan.reason, false};
			// Granted context is a fact about the CURRENT load only.
			if (serving) state.effective_context_tokens = entry.plan.requested_context_tokens;
		}
	}
	state.planned_context_tokens = planned_context_tokens(model_id, capabilities);
	return state;
}

} // namespace singularity::local_inference
