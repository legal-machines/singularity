#include "LocalInference/LocalInferenceService/LocalInferenceService.hpp"

#include "LocalInference/RuntimeClosure/RuntimeClosure.hpp"
#include "LocalModel/LocalThroughput/LocalThroughput.hpp"
#include "Node/DeviceCapabilities/DeviceCapabilities.hpp"
#include "Platform/AppPaths/AppPaths.hpp"
#include "Platform/HttpClient/HttpClient.hpp"
#include "Platform/LoopbackSocket/LoopbackSocket.hpp"

#include <atomic>
#include <stdexcept>
#include <system_error>

namespace singularity::local_inference {
namespace {

const char *build_platform() {
#if defined(__APPLE__)
	const char *system = "macos";
#elif defined(_WIN32)
	const char *system = "windows";
#else
	const char *system = "linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__arm64__)
	static const std::string name = std::string(system) + "-arm64";
#else
	static const std::string name = std::string(system) + "-x86_64";
#endif
	return name.c_str();
}

// Clears the abort a call set on its work's cancel token, once the call ends:
// a Stop that comes after it never reaches a call that is gone.
struct AbortScope {
	std::shared_ptr<platform::HttpCancelToken> token;
	~AbortScope() {
		if (token != nullptr) token->clear_abort();
	}
};

// Arms the thread's cancel token (a person's Stop of the work this call
// belongs to, as for a provider request) to set `stop`, which ends the call's
// request to the runtime at once. Throws when the work was stopped already.
void arm_stop(AbortScope &scope, const std::shared_ptr<platform::HttpCancelToken> &token,
		const std::shared_ptr<std::atomic<bool>> &stop) {
	if (token == nullptr) return;
	if (!token->set_abort([stop] { stop->store(true); }))
		throw platform::HttpTransportError("The local generation was stopped before it began.", false, true);
	scope.token = token;
}

} // namespace

std::uint32_t local_generation_timeout_seconds(double slowest_completed_generation_seconds) noexcept {
	// What Singularity waited before it could measure anything. Kept exactly, so a
	// first launch behaves as it always did.
	constexpr std::uint32_t kUnmeasured = 480;
	// A generation that has already taken twice as long as the slowest one this
	// machine ever completed is not slow, it is not converging - and waiting
	// longer than this would push a single physical call past the product's own
	// end-to-end ceiling with nothing left for loading, the document write and
	// the rest of the turn.
	constexpr std::uint32_t kNeverLongerThan = 900;
	if (!(slowest_completed_generation_seconds > 0.0)) return kUnmeasured;
	const double doubled = slowest_completed_generation_seconds * 2.0;
	if (doubled <= static_cast<double>(kUnmeasured)) return kUnmeasured;
	if (doubled >= static_cast<double>(kNeverLongerThan)) return kNeverLongerThan;
	return static_cast<std::uint32_t>(doubled);
}

LocalInferenceService &LocalInferenceService::production_instance() {
	// Never destroyed: its runtimes are child processes that end with this
	// process by any means (Platform/ChildProcess), and static teardown order
	// must not decide when a model stops.
	static auto *const instance = [] {
		const auto data = platform::application_data_directory();
		// Production discovery has one truth: the app-owned folder the package
		// manager installs into. No environment variable can make an unmanaged
		// file appear installed.
		const auto models = data / "Models" / "generative";
		std::error_code error;
		std::filesystem::create_directories(models, error);
		return new LocalInferenceService(std::make_unique<GenerativeModelService>(models, data));
	}();
	return *instance;
}

LocalInferenceService::LocalInferenceService(std::unique_ptr<GenerativeModelService> models)
	: models_(std::move(models)) {}

std::function<bool(const std::string &)> LocalInferenceService::unload_callback() {
	return [this](const std::string &model_id) { return unload(model_id); };
}

void LocalInferenceService::discover_and_register(hardware::LocalModelRegistry &registry,
		const std::map<std::string, std::filesystem::path> &managed_install_paths) {
	const std::lock_guard<std::mutex> lock(mutex_);
	models_->discover_and_register(registry, managed_install_paths, unload_callback());
}

void LocalInferenceService::rediscover_after_package_change(hardware::LocalModelRegistry &registry,
		const std::map<std::string, std::filesystem::path> &managed_install_paths) {
	// Rediscovery stops every runtime: refused while a call is running, rather
	// than ending it under the person waiting. Asked under the lock every call
	// holds while it begins its use, so no call can begin between this answer
	// and the reset.
	const std::lock_guard<std::mutex> lock(mutex_);
	if (uses_.any_in_use())
		throw std::runtime_error("A local model is answering right now. Try again once it finishes.");
	models_->reset();
	models_->discover_and_register(registry, managed_install_paths, unload_callback());
}

bool LocalInferenceService::contains(const std::string &model_id) const { return models_->contains(model_id); }

std::vector<std::string> LocalInferenceService::known_model_ids() const { return models_->model_ids(); }

std::optional<std::string> LocalInferenceService::runtime_model_id_for(const std::string &model_id) const {
	return models_->artifact_id_for(model_id);
}

RuntimeIdentity LocalInferenceService::runtime_identity() const {
	RuntimeIdentity identity;
	const RuntimeClosureInfo &closure = packaged_runtime_closure();
	identity.available = closure.packaged;
	identity.version = closure.build;
#if defined(NDEBUG)
	identity.build_type = "Release";
#else
	identity.build_type = "Debug";
#endif
	identity.platform = build_platform();
	const auto capabilities = hardware::detect_hardware_capabilities();
	identity.system_memory_bytes = capabilities.total_system_memory_bytes;
	// Named only when local models really run on it: an adapter the runtime
	// was not built for is a fact about the machine, not where models run.
	if (closure.accelerator && capabilities.supports(*closure.accelerator))
		identity.accelerator_identity = capabilities.accelerator_identity;
	return identity;
}

void LocalInferenceService::load(const std::string &model_id) {
	const std::lock_guard<std::mutex> lock(mutex_);
	models_->load(model_id);
}

bool LocalInferenceService::unload(const std::string &model_id) noexcept {
	// A model serving a call is never torn down under it: the caller is told
	// nothing was freed rather than losing the generation in flight. Asked
	// under the lock every call holds while it begins its use, so no call can
	// begin between this answer and the teardown.
	const std::lock_guard<std::mutex> lock(mutex_);
	if (uses_.in_use(model_id)) return false;
	models_->unload(model_id);
	return true;
}

std::vector<std::string> LocalInferenceService::models_in_use() const { return uses_.models_in_use(); }

GenerativeResult LocalInferenceService::generate(const std::string &model_id, GenerativeRequest request) {
	// Work stopped before it reached the model loads nothing and starts nothing.
	const std::shared_ptr<platform::HttpCancelToken> token = platform::current_http_cancel_token();
	if (token != nullptr && token->is_cancelled())
		throw platform::HttpTransportError("The local generation was stopped before it began.", false, true);

	LocalModelUseRegistry::Hold use;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		models_->load(model_id);
		// Held for the whole call, while this service's own lock is released:
		// no unload, reinstall, removal or memory release may stop this
		// model's runtime until the call returns.
		use = uses_.begin_use(model_id);
	}

	// How long this generation may take, from what this machine has shown
	// (local_generation_timeout_seconds()); 480 seconds until a first real
	// generation has been measured.
	request.timeout_seconds = static_cast<int>(local_generation_timeout_seconds(
		local_model::slowest_completed_local_generation_seconds()));
	const auto stop = std::make_shared<std::atomic<bool>>(false);
	request.stop = stop;
	AbortScope abort_scope;
	arm_stop(abort_scope, token, stop);
	const auto stopped = [&] { return stop->load() || (token != nullptr && token->is_cancelled()); };

	// A bounded (one) retry of the IDENTICAL request, never a modified one,
	// scoped to exactly one evidenced failure class: "reason=compute_error"
	// (safe_generation_rejection_message()) is llama.cpp's generic
	// constrained-decoding failure, a non-deterministic defect of its grammar
	// stack where an identical request can succeed or abort depending on
	// token-boundary timing. A real local-only acceptance run observed it
	// after minutes of progress on a request identical in shape to ones that
	// succeeded moments earlier. out_of_memory and context_capacity are
	// deterministic resource facts a retry cannot change, so they are never
	// retried; neither is a generation a person stopped.
	for (int attempt = 0; attempt < 2; ++attempt) {
		try {
			GenerativeResult result = models_->generate(model_id, request);
			// What a generation returns after a Stop is never handed on as the
			// answer.
			if (stopped()) throw platform::HttpTransportError("The local generation was stopped.", false, true);
			return result;
		} catch (const platform::HttpTransportError &) {
			throw;
		} catch (const std::exception &error) {
			if (stopped()) throw platform::HttpTransportError("The local generation was stopped.", false, true);
			// The transport's own typed timeout is what a caller branches on
			// (InferenceExecutor's bounded repair path), never the message.
			if (const auto *transport = dynamic_cast<const platform::LoopbackHttpError *>(&error);
				transport != nullptr && transport->kind() == platform::LoopbackHttpError::Kind::TimedOut)
				throw platform::HttpTransportError(error.what(), true, false);
			if (attempt == 0 && std::string(error.what()).find("reason=compute_error") != std::string::npos) continue;
			throw;
		}
	}
	throw std::runtime_error("Local AI could not generate a response.");
}

GenerativeRequestMeasurement LocalInferenceService::measure(const std::string &model_id, GenerativeRequest request) {
	const std::shared_ptr<platform::HttpCancelToken> token = platform::current_http_cancel_token();
	if (token != nullptr && token->is_cancelled())
		throw platform::HttpTransportError("The local generation was stopped before it began.", false, true);

	LocalModelUseRegistry::Hold use;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		models_->load(model_id);
		use = uses_.begin_use(model_id);
	}
	const auto stop = std::make_shared<std::atomic<bool>>(false);
	request.stop = stop;
	AbortScope abort_scope;
	arm_stop(abort_scope, token, stop);
	try {
		return models_->measure(model_id, request);
	} catch (const platform::HttpTransportError &) {
		throw;
	} catch (const std::exception &) {
		if (stop->load() || (token != nullptr && token->is_cancelled()))
			throw platform::HttpTransportError("The local generation was stopped.", false, true);
		throw;
	}
}

} // namespace singularity::local_inference
