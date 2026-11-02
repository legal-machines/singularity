#pragma once

#include "LocalInference/GenerativeModelService/GenerativeModelService.hpp"
#include "LocalInference/LocalModelUse/LocalModelUse.hpp"
#include "LocalModel/LocalModelRegistry/LocalModelRegistry.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace singularity::local_inference {

// The local inference runtime this build carries and the machine it runs on,
// as Singularity reports it (localModels.getRuntimeState, a local result's
// runtime fields).
struct RuntimeIdentity {
	// Whether this build carries a runtime at all.
	bool available = false;
	// The llama.cpp build the runtime is made from, e.g. "b10516".
	std::string version;
	std::string build_type;
	// "windows-x86_64", "macos-arm64", "linux-x86_64".
	std::string platform;
	// The accelerator local models run on here, empty when they run on the
	// CPU alone.
	std::string accelerator_identity;
	std::uint64_t system_memory_bytes = 0;
};

// How long a local generation may take before Singularity stops waiting for it.
//
// A ceiling is a statement about the WORST case, so it is calculated from the
// slowest speed this machine has actually shown, never from an unthrottled
// benchmark. It used to be a constant derived from "~104 tokens a second of
// prefill"; a real governed run on the same hardware measured 17.5, and a
// genuine turn was killed at the ceiling as though it had stopped converging.
//
// Pure, so it can be reasoned about and tested without a model: twice the
// slowest generation this machine has actually completed, bounded so it can
// never wait forever and never drop below the figure Singularity used before any
// measurement existed. Zero means nothing has been measured yet, and that
// earlier figure stands unchanged.
[[nodiscard]] std::uint32_t local_generation_timeout_seconds(
	double slowest_completed_generation_seconds) noexcept;

// What the rest of Singularity asks of local inference: the models it found,
// loading and freeing them, generating and measuring. On top of the models
// themselves (GenerativeModelService) it keeps the product's own guarantees:
// a model serving a call is never torn down under it, a person's Stop ends a
// generation in flight, a known transient runtime failure is retried once,
// and how long a generation may take follows what this machine has shown.
class LocalInferenceService final {
public:
	// The service of this process, on Singularity's own data folder: models
	// are discovered from <data>/Models/generative, the folder the package
	// manager installs into. Never destroyed: its runtimes end with the
	// process (Platform/ChildProcess).
	static LocalInferenceService &production_instance();

	explicit LocalInferenceService(std::unique_ptr<GenerativeModelService> models);
	LocalInferenceService(const LocalInferenceService &) = delete;
	LocalInferenceService &operator=(const LocalInferenceService &) = delete;

	// Registers every model in the folder in `registry`; `managed_install_
	// paths` names, by the id each is known by, the files Singularity manages
	// (installed packages, files the person chose). Repeatable: a model
	// already known keeps its registration.
	void discover_and_register(hardware::LocalModelRegistry &registry,
		const std::map<std::string, std::filesystem::path> &managed_install_paths = {});
	// After a package was installed or removed: every runtime stops and the
	// folder is discovered afresh. Refused while a model is answering.
	void rediscover_after_package_change(hardware::LocalModelRegistry &registry,
		const std::map<std::string, std::filesystem::path> &managed_install_paths = {});

	bool contains(const std::string &model_id) const;
	std::vector<std::string> known_model_ids() const;
	// The id of the model file itself (its artifact id), recorded beside a
	// local result.
	std::optional<std::string> runtime_model_id_for(const std::string &model_id) const;
	RuntimeIdentity runtime_identity() const;

	void load(const std::string &model_id);
	// Frees the model unless a call of its own is running. Returns false then,
	// so a caller never reports memory it did not free and a generation in
	// flight is never ended under a person waiting for it.
	bool unload(const std::string &model_id) noexcept;
	[[nodiscard]] std::vector<std::string> models_in_use() const;

	// Generates on the thread's work: a Stop of that work (its
	// platform::HttpCancelToken) ends the generation at once, and what comes
	// back after a Stop is never handed on - the call throws
	// platform::HttpTransportError with `cancelled` set. A generation that
	// exceeds its time limit throws HttpTransportError with `timed_out` set.
	GenerativeResult generate(const std::string &model_id, GenerativeRequest request);
	// Non-generating - see IGenerativeRuntimeAdapter::measure().
	GenerativeRequestMeasurement measure(const std::string &model_id, GenerativeRequest request);

private:
	std::function<bool(const std::string &)> unload_callback();

	// Held while a model is loaded or a call begins its use of it, and while a
	// teardown asks whether the model is in use: no call can begin between a
	// teardown's answer and the teardown itself.
	mutable std::mutex mutex_;
	LocalModelUseRegistry uses_;
	std::unique_ptr<GenerativeModelService> models_;
};

} // namespace singularity::local_inference
