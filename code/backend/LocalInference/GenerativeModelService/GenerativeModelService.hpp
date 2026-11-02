#pragma once

#include "LocalInference/GenerativeRuntime/GenerativeRuntime.hpp"
#include "LocalInference/ModelInspector/ModelInspector.hpp"
#include "LocalModel/LocalModelRegistry/LocalModelRegistry.hpp"
#include "Node/HardwareCalibration/HardwareCalibration.hpp"
#include "Node/MachineResourceGovernor/MachineResourceGovernor.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace singularity::local_inference {

// The min/recommended resident-memory split for one already-inspected
// artifact - pure, so the min-vs-recommended MoE dynamic-expert-residency
// split is unit-testable. The two stay distinct: a Model Library card's "what
// this uses" and the admission gate's "can this load at all" are different
// questions (GenerativeModelService.test.cpp).
hardware::ResourceRequirement resource_requirement_for(const ModelExecutionProfile &profile);

// The MachineResourceGovernor request for loading `plan`. On unified memory
// everything the plan owns is host memory; on a discrete accelerator the
// host and accelerator pools are separate and each is charged only the
// consumers the plan places in it (AdaptiveMemoryConsumer::pool).
hardware::MachineResourceRequest resource_request_for(const std::string &owner,
	const AdaptiveExecutionPlan &plan, bool unified_memory);

// The real context window (in tokens) a load requests for this model on this
// machine right now: the model's own inspected native maximum when the real
// memory-feasibility check (the same one plan_model_execution() makes for
// every candidate) says it safely fits, backing off only as far as that check
// requires, down to a safe floor (4096) when nothing larger fits or the
// native capacity is unknown. Pure with respect to disk and processes.
std::uint64_t resolve_requested_context_tokens(
	const ModelExecutionProfile &profile,
	const hardware::HardwareCapabilities &planning_capabilities,
	const hardware::HardwareCalibration &calibration);

// The id a discovered model file is known by everywhere (AI Setup
// assignments, usage records): "local-gguf-" and the FNV-1a 64-bit hash of
// "<canonical path>:<architecture>:<size in bytes>". Persisted, so it never
// changes for the same file.
std::string stable_artifact_id(const std::filesystem::path &path, const ModelExecutionProfile &profile);

// Local inference's models: discovered from one folder, inspected without
// loading, planned for this machine, loaded under the machine resource
// governor into a runtime of their own, and generated with.
class GenerativeModelService final {
public:
	// Starts the runtime a loaded model runs in; a test stands in for
	// llama-server through it.
	using RuntimeFactory = std::function<std::shared_ptr<IGenerativeRuntimeAdapter>()>;

	// `model_search_path` is scanned (recursively) for installed *.gguf
	// files. `application_data_path` is Singularity's own data folder: the
	// hardware profile planning reads (HardwareProfile/) and the runtime's
	// log (Logs/) live there. No runtime_factory: llama-server.
	GenerativeModelService(std::filesystem::path model_search_path,
		std::filesystem::path application_data_path, RuntimeFactory runtime_factory = {});
	~GenerativeModelService();
	GenerativeModelService(const GenerativeModelService &) = delete;
	GenerativeModelService &operator=(const GenerativeModelService &) = delete;

	// Registers every model file in the search folder in `registry`
	// (upsert_model), with this service's live state. A file that is one of
	// `managed_install_paths` - a package Singularity installed, a file the
	// person chose, by the id it is known by - keeps that id and path; any
	// other file is known by stable_artifact_id(). `unload_model` is what the
	// registry calls to free a model (it may refuse while the model answers).
	// Models already known are left as they are.
	void discover_and_register(hardware::LocalModelRegistry &registry,
		const std::map<std::string, std::filesystem::path> &managed_install_paths,
		std::function<bool(const std::string &)> unload_model);
	// Stops every runtime and forgets every model, for a fresh discovery.
	void reset() noexcept;

	bool contains(const std::string &model_id) const;
	std::vector<std::string> model_ids() const;
	// The stable_artifact_id() of the file a model id names.
	std::optional<std::string> artifact_id_for(const std::string &model_id) const;

	// Loads the model unless its runtime is already running.
	void load(const std::string &model_id);
	void unload(const std::string &model_id) noexcept;
	GenerativeResult generate(const std::string &model_id, const GenerativeRequest &request);
	// Non-generating measurement - see IGenerativeRuntimeAdapter::measure().
	// Goes through the same load/pressure/governor path generate() does: a
	// measurement still needs the real model loaded and the runtime healthy.
	GenerativeRequestMeasurement measure(const std::string &model_id, const GenerativeRequest &request);

	// PLANNED / FEASIBLE CONTEXT for this model on this machine right now:
	// exactly what a load would request, computed without loading and without
	// ever starting a hardware calibration (only a cached one is used). For a
	// model loaded right now it is the plan that load was made with - planned
	// again, it would be charged for the memory its own runtime occupies. A
	// model that still holds its lease but is not running plans with that lease
	// left out. std::nullopt when the model is unknown or no cached calibration
	// exists yet - genuinely unknown, never a constant.
	std::optional<std::uint64_t> planned_context_tokens(const std::string &model_id) const;
	hardware::LocalModelLiveState live_state(const std::string &model_id,
		const hardware::HardwareCapabilities &capabilities) const;

private:
	struct Entry {
		std::filesystem::path path;
		std::string artifact_id;
		ModelExecutionProfile profile;
		AdaptiveExecutionPlan plan;
		std::shared_ptr<IGenerativeRuntimeAdapter> runtime;
		std::optional<hardware::MachineResourceGovernor::Lease> resource_lease;
		hardware::LocalModelLoadState state = hardware::LocalModelLoadState::Unloaded;
		// Why the model's last load or generation failed; empty once it serves.
		std::string error;
		std::mutex runtime_mutex;
		std::atomic<bool> cancel_requested{false};
	};

	// The exact inputs every context/plan decision is made from - one
	// function, so a pre-load capability query and the real load can never
	// disagree about the machine they plan for. `allow_calibration_
	// measurement` false (capability queries) returns std::nullopt instead of
	// measuring when no valid cached calibration exists. `excluding_lease_id`
	// names the ONE governor lease left out of the envelope - the queried
	// model's own lease, matching load_entry(), which releases exactly that
	// lease before it plans. 0 excludes nothing.
	struct PlanningInputs {
		// The ONE hardware snapshot this decision derives from.
		hardware::HardwareCapabilities capabilities;
		hardware::HardwareCapabilities planning_capabilities;
		hardware::HardwareCalibration calibration;
	};
	std::optional<PlanningInputs> resolve_planning_inputs(const hardware::HardwareCapabilities &capabilities,
		bool allow_calibration_measurement, std::uint64_t excluding_lease_id = 0) const;
	std::optional<std::uint64_t> planned_context_tokens(const std::string &model_id,
		const hardware::HardwareCapabilities &capabilities) const;
	std::shared_ptr<Entry> entry_for(const std::string &model_id) const;
	// The owner name a model's governor lease is acquired under -
	// informational; exclusion is always by lease id.
	static std::string resource_lease_owner(const Entry &entry);
	// Every change of an Entry's lease goes through here, under mutex_; the
	// previous lease is released after mutex_ is dropped.
	void replace_resource_lease(Entry &entry,
		std::optional<hardware::MachineResourceGovernor::Lease> lease);
	void load_entry(const std::shared_ptr<Entry> &entry);
	// Before a generation or measurement: loads the model if its runtime is
	// not running and fails closed under Critical memory pressure.
	void prepare_to_serve(const std::shared_ptr<Entry> &entry);
	void record_error(Entry &entry, const std::string &error);

	mutable std::mutex mutex_;
	std::unordered_map<std::string, std::shared_ptr<Entry>> entries_;
	std::filesystem::path model_search_path_;
	std::filesystem::path application_data_path_;
	RuntimeFactory runtime_factory_;
	hardware::MachineResourceGovernor resource_governor_;
};

} // namespace singularity::local_inference
