#include "LocalModel/UserModelFiles/UserModelFiles.hpp"
#include "LocalModel/LocalThroughput/LocalThroughput.hpp"
#include "Privacy/PrivacyActions/PrivacyActions.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"

#include "Platform/Trace/Trace.hpp"

#include "Routing/AiSetup/AiSetup.hpp"
#include "LocalInference/LocalInferenceService/LocalInferenceService.hpp"
#include "LocalModel/ExecutionCompatibility/ExecutionCompatibility.hpp"
#include "LocalModel/LocalModelRegistry/LocalModelRegistry.hpp"
#include "LocalModel/ModelPackageManager/ModelPackageManager.hpp"
#include "LocalModel/ResourceBenchmark/ResourceBenchmark.hpp"
#include "Node/DeviceCapabilities/DeviceCapabilities.hpp"
#include "Platform/AppPaths/AppPaths.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/HttpClient/HttpClient.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"
#include "Provider/CredentialStore/CredentialStore.hpp"
#include "LocalModel/Models/PapinianNER/Gliner2Adapter/Gliner2Adapter.hpp"
#include "LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.hpp"
#include "Provider/Types/TaskError.hpp"
#include "Routing/ProtectedInference/ProtectedInference.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "ExternalResearch/ResearchTools/ResearchTools.hpp"
#include "Tool/OperationAuthority/OperationAuthority.hpp"
#include "ExternalResearch/ResearchToolLoop/ResearchToolLoop.hpp"
#include "PapinianGuard/ProtectedComputation/ComputeBinding.hpp"
#include "PapinianGuard/ProtectedOperations/ProtectedOperations.hpp"
#include "PapinianGuard/ProtectedTables/ProtectedTables.hpp"
#include "PapinianGuard/ProtectedRelations/ProtectedRelations.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace singularity::routing {
namespace {

// The request's turn: its prompt - in parts, where the caller gave it so - and
// how long a reasoning model thinks on it.
ModelVisibleTurn request_turn(const InferenceRequest &request) {
	ModelVisibleTurn turn = request.prompt_parts.empty() ? conversational_model_turn(request.prompt)
		: conversational_model_turn(request.prompt_instruction, request.prompt_parts);
	turn.reasoning_effort = request.reasoning_effort;
	turn.is_conversation = request.is_conversation;
	return turn;
}

// ---- A structured-output grammar a provider will not compile ---------------
//
// Singularity's professional contract is sent as a strict JSON schema, and some
// providers refuse to compile one this large before any work happens:
// Anthropic says "The compiled grammar is too large", Google says the schema
// "produces a constraint that has too many states". Choosing such a model
// used to end the request there. The contract is the same either way; what
// differs is only whether the provider enforces it or Singularity does. So a
// refused grammar is answered once by sending the contract as a description
// the model follows, and Singularity validates what comes back exactly as it
// always does. The model is remembered for the life of the process, so the
// next request does not pay for the same refusal again.
bool refuses_structured_grammar(const std::string &message) {
	std::string lowered;
	lowered.reserve(message.size());
	for (const char character : message)
		lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
	return lowered.find("grammar") != std::string::npos
		|| lowered.find("too many states") != std::string::npos
		|| lowered.find("schema") != std::string::npos;
}

std::mutex &grammar_refusal_mutex() {
	static std::mutex mutex;
	return mutex;
}

std::set<std::string> &models_refusing_grammar() {
	static std::set<std::string> models;
	return models;
}

bool refused_grammar_before(const std::string &model) {
	const std::lock_guard<std::mutex> lock(grammar_refusal_mutex());
	return models_refusing_grammar().count(model) != 0;
}

void remember_grammar_refusal(const std::string &model) {
	const std::lock_guard<std::mutex> lock(grammar_refusal_mutex());
	models_refusing_grammar().insert(model);
}

// The same memory for a model that turned a reasoning-effort parameter away:
// it is sent without one for the life of the process.
std::set<std::string> &models_refusing_reasoning_effort() {
	static std::set<std::string> models;
	return models;
}

bool refused_reasoning_effort_before(const std::string &model) {
	const std::lock_guard<std::mutex> lock(grammar_refusal_mutex());
	return models_refusing_reasoning_effort().count(model) != 0;
}

void remember_reasoning_effort_refusal(const std::string &model) {
	const std::lock_guard<std::mutex> lock(grammar_refusal_mutex());
	models_refusing_reasoning_effort().insert(model);
}

// And a model whose endpoint will not answer without thinking: asked for none,
// it is asked for the least from then on (providers::requires_reasoning()).
std::set<std::string> &models_requiring_reasoning() {
	static std::set<std::string> models;
	return models;
}

bool requires_reasoning_before(const std::string &model) {
	const std::lock_guard<std::mutex> lock(grammar_refusal_mutex());
	return models_requiring_reasoning().count(model) != 0;
}

void remember_reasoning_required(const std::string &model) {
	const std::lock_guard<std::mutex> lock(grammar_refusal_mutex());
	models_requiring_reasoning().insert(model);
}

// A model following a described contract, rather than an enforced one, may
// still wrap its object in a code fence or put a sentence before it. The
// object is taken out of what surrounds it; text with no object in it is
// left as it came, and is read as the plain answer it is.
std::string bare_json_object(const std::string &text) {
	if (nlohmann::json::parse(text, nullptr, false).is_object()) return text;
	const std::size_t open = text.find('{');
	const std::size_t close = text.rfind('}');
	if (open == std::string::npos || close == std::string::npos || close <= open) return text;
	const std::string inside = text.substr(open, close - open + 1);
	return nlohmann::json::parse(inside, nullptr, false).is_object() ? inside : text;
}

// What a strict provider would have enforced and a described contract only
// asks for: every property the schema requires is present, and a value the
// schema says is a string or a list is one, even when the model wrote null.
// Nothing is invented - an empty string for a string, an empty list for a
// list, and a missing true/false or number is left out so the reader's own
// default applies. An object the model left null stays null: a whole object
// manufactured from nothing would say something the model never said.
bool schema_admits(const nlohmann::json &schema, const std::string &type) {
	if (!schema.is_object() || !schema.contains("type")) return true;
	const nlohmann::json &declared = schema.at("type");
	if (declared.is_string()) return declared.get<std::string>() == type;
	if (declared.is_array())
		for (const nlohmann::json &each : declared)
			if (each.is_string() && each.get<std::string>() == type) return true;
	return false;
}

void conform_to_schema(nlohmann::json &value, const nlohmann::json &schema) {
	if (!schema.is_object()) return;
	if (value.is_null() && !schema_admits(schema, "null")) {
		if (schema_admits(schema, "string")) value = "";
		else if (schema_admits(schema, "array")) value = nlohmann::json::array();
	}
	if (value.is_object() && schema.contains("properties") && schema.at("properties").is_object()) {
		const nlohmann::json &properties = schema.at("properties");
		if (schema.contains("required") && schema.at("required").is_array())
			for (const nlohmann::json &key : schema.at("required"))
				if (key.is_string() && !value.contains(key.get<std::string>())) value[key.get<std::string>()] = nullptr;
		std::vector<std::string> leftOut;
		for (auto member = value.begin(); member != value.end(); ++member) {
			if (!properties.contains(member.key())) continue;
			const nlohmann::json &memberSchema = properties.at(member.key());
			conform_to_schema(member.value(), memberSchema);
			if (member.value().is_null() && !schema_admits(memberSchema, "null")
					&& !schema_admits(memberSchema, "object"))
				leftOut.push_back(member.key());
		}
		for (const std::string &key : leftOut) value.erase(key);
	}
	if (value.is_array() && schema.contains("items"))
		for (nlohmann::json &item : value) conform_to_schema(item, schema.at("items"));
}

std::string trim(const std::string &value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::filesystem::path production_model_root() {
  if (const auto *override_path = std::getenv("SINGULARITY_PAPINIAN_MODEL_DIR")) {
    if (*override_path != '\0') return override_path;
  }
  // Retain the established opt-in probe variable while the verified package
  // is not yet a release-bundled artifact.
  if (const auto *probe_path = std::getenv("SINGULARITY_PAPINIAN_REAL_MODEL_DIR")) {
    if (*probe_path != '\0') return probe_path;
  }

  return platform::application_data_directory() / "Models" / "papinian-ner" /
         "1.0.0";
}

constexpr std::uint64_t kGiB = 1024ULL * 1024 * 1024;

// Papinian runs on the CPU. The pinned package is fp16, and the bundled ONNX
// Runtime's CoreML provider (NeuralNetwork format, what lib_v2.rs registers
// when FORCE_CPU is unset) accepts no fp16 operator: across the eight
// sessions it took only 148 integer Slice nodes, so all real work already ran
// on the CPU while the provider added about 0.2 s to every launch. Measured
// 2026-09-23 on this package: CPU and CoreML give byte-identical detections
// and the same steady-state latency, single and batched. CoreML's MLProgram
// format does take the encoder, but loaded in 4-6 s, inferred 30-150% slower
// and changed detections. Reconsider only for a package whose operators an
// accelerator provider actually executes.
std::vector<hardware::ExecutionBackend> papinian_preferred_backends() {
  return {hardware::ExecutionBackend::Cpu};
}

// Rounded from measured evidence (docs/alpha/phase-03-papinian.md's "Bundled
// runtime and controlled native benchmark" / "Same-schema equivalence"
// sections recorded ~1.1-1.75 GB peak RSS across CoreML and CPU runs), not
// invented.
hardware::ResourceRequirement papinian_resource_requirement() {
  hardware::ResourceRequirement requirement;
  requirement.min_ram_bytes = 1 * kGiB;
  requirement.recommended_ram_bytes = 2 * kGiB;
  requirement.min_accelerator_memory_bytes = 2 * kGiB;
  return requirement;
}

void force_cpu_execution() {
#if defined(_WIN32)
  _putenv_s("FORCE_CPU", "1");
#else
  setenv("FORCE_CPU", "1", 1);
#endif
}

struct PapinianExecutionDecision {
  std::optional<hardware::ExecutionPlan> plan;
  // Set instead of plan when nothing viable was found - e.g. no ONNX
  // Runtime bundled at all on this platform yet (see
  // hardware_capabilities_windows.cpp / _linux.cpp).
  std::optional<std::string> unavailable_reason;
};

// Decided once, before the model is ever loaded (the model loads lazily
// exactly once via VerifiedGliner2Runtime's sticky engine() - see
// gliner2_runtime.cpp - so the backend choice must land before that first
// load, not be re-decided per request). Never throws: a planning failure is
// recorded as unavailable_reason rather than propagating from a
// function-local static initializer, which would otherwise retry
// construction (and its side effect, force_cpu_execution()) on every call.
const PapinianExecutionDecision &papinian_execution_decision() {
  static const PapinianExecutionDecision decision = [] {
    try {
      auto plan = hardware::choose_execution_plan(papinian_preferred_backends(),
                                                   papinian_resource_requirement(),
                                                   hardware::detect_hardware_capabilities());
      // Only CPU and AppleAccelerator exist as real choices in the bundled
      // Rust bridge today; FORCE_CPU is its own existing escape hatch (see
      // lib_v2.rs) for skipping the CoreML provider probe entirely.
      if (plan.backend != hardware::ExecutionBackend::AppleAccelerator) {
        force_cpu_execution();
      }
      return PapinianExecutionDecision{std::move(plan), std::nullopt};
    } catch (const std::exception &error) {
      return PapinianExecutionDecision{std::nullopt, std::string(error.what())};
    }
  }();
  return decision;
}

hardware::LocalModelDescriptor papinian_local_model_descriptor() {
  hardware::LocalModelDescriptor descriptor;
  descriptor.id = "papinian-ner";
  descriptor.display_name = "Papinian NER";
  descriptor.purpose = "Local sensitive-data detection for Protected API";
  descriptor.role = "Anonymization";
  descriptor.family = "Papinian";
  descriptor.developer = "Legal Machines";
  descriptor.version = local_models::papinian_package_version();
  descriptor.install_path = production_model_root();
  descriptor.preferred_backends = papinian_preferred_backends();
  descriptor.resource_requirement = papinian_resource_requirement();
  // Real, known facts about this package's own format - GLiNER2 is a
  // non-generative encoder (span/entity extraction), never an
  // autoregressive dense/MoE transformer, so those execution strategies
  // never apply to it; fp16 is what ModelPackageManager's own expected
  // archive contents (encoder_fp16.onnx, etc.) actually ship.
  descriptor.execution_profile.architecture_kind = hardware::ModelArchitectureKind::EncoderExtraction;
  descriptor.execution_profile.architecture_description =
      "Encoder-based span/entity extraction (GLiNER2)";
  descriptor.execution_profile.precision = "fp16";
  descriptor.execution_profile.model_state_bytes = local_models::papinian_archive_bytes();
  // No KV/recurrent state and no expert routing - a single stateless
  // forward pass per request, so requires_runtime_state and the MoE fields
  // all stay at their honest defaults (false/unset).
  descriptor.required = true;
  return descriptor;
}

hardware::LocalModelDescriptor generative_catalog_descriptor(
	const local_models::ModelPackageManifest &package) {
	hardware::LocalModelDescriptor descriptor;
	descriptor.id = package.runtime_model_id;
	descriptor.display_name = package.display_name;
	descriptor.purpose = "Private local text generation and structured Context creation";
	descriptor.role = "Generation";
	descriptor.family = "Qwen";
	descriptor.developer = "Qwen / ggml-org";
	descriptor.version = package.source_revision.substr(0, 12);
	descriptor.install_path = package.installed_path;
	descriptor.preferred_backends = {hardware::ExecutionBackend::Cpu};
	descriptor.resource_requirement.min_ram_bytes = package.artifact_bytes + 256ULL * 1024 * 1024;
	descriptor.resource_requirement.recommended_ram_bytes =
		descriptor.resource_requirement.min_ram_bytes;
	descriptor.execution_profile.architecture_kind =
		hardware::ModelArchitectureKind::MixtureOfExperts;
	descriptor.execution_profile.architecture_description = "Mixture of Experts transformer";
	descriptor.execution_profile.precision = "Q4_K_M";
	descriptor.execution_profile.quantization = "Q4_K_M";
	descriptor.execution_profile.model_state_bytes = package.artifact_bytes;
	descriptor.execution_profile.requires_runtime_state = true;
	descriptor.execution_profile.expert_count = 128;
	descriptor.execution_profile.active_experts_per_token = 8;
	descriptor.required = false;
	return descriptor;
}

struct ProductionPapinianResources {
  std::shared_ptr<protected_api::papinian::VerifiedGliner2Runtime> runtime;
  protected_api::papinian::PapinianGuard *guard = nullptr;
};

ProductionPapinianResources &production_papinian_resources() {
  // Deliberately never destroyed. The vendored runtime does not tolerate
  // process-exit static teardown; the OS reclaims these process resources.
  static auto *const resources = [] {
    auto *result = new ProductionPapinianResources;
    // The runtime calls its factory once the installed package has been
    // verified; around it the running warm-up reports its real steps: opening
    // the model, then preparing it (re-verification and the first protection
    // pass). A load outside a warm-up reports nothing (see set_phase()).
    result->runtime = std::make_shared<protected_api::papinian::VerifiedGliner2Runtime>(
        protected_api::papinian::pinned_fastino_v2_model_package(production_model_root()),
        [](const protected_api::papinian::Gliner2ModelPackage &package) {
          papinian_warmup_tracker().set_phase(ModelWarmupPhase::LoadingModel);
          auto engine = protected_api::papinian::make_native_gliner2_engine(package);
          papinian_warmup_tracker().set_phase(ModelWarmupPhase::Preparing);
          return engine;
        });
    auto adapter =
        std::make_shared<protected_api::papinian::Gliner2SemanticModelAdapter>(result->runtime);
    result->guard = new protected_api::papinian::PapinianGuard(std::move(adapter));
    return result;
  }();
  return *resources;
}

// Where real, locally-measured warm-up/memory evidence is cached and
// compared against the previous run (docs/LOCAL_INFERENCE.md).
// A small local JSON file, never network state.
std::filesystem::path papinian_benchmark_baseline_path() {
  return platform::application_data_directory() / "HardwareProfile" / "papinian-benchmark.json";
}

struct PapinianResourceBenchmarkState {
  std::mutex mutex;
  std::optional<hardware::BenchmarkComparison> last_comparison;
  // What the last finished warm-up of this model version on this backend
  // took, read when the running warm-up began (papinian_expected_warm_up_ms()).
  std::optional<double> expected_warm_up_ms;
};

PapinianResourceBenchmarkState &papinian_resource_benchmark_state() {
  static PapinianResourceBenchmarkState state;
  return state;
}

hardware::LocalModelLiveState papinian_local_model_live_state() {
  hardware::LocalModelLiveState state;
  const auto status = papinian_warmup_tracker().status();
  switch (status.state) {
    case ModelLifecycleState::Loading:
      state.load_state = hardware::LocalModelLoadState::Loading;
      break;
    case ModelLifecycleState::Ready:
      state.load_state = hardware::LocalModelLoadState::Ready;
      break;
    case ModelLifecycleState::Unloaded:
      state.load_state = hardware::LocalModelLoadState::Unloaded;
      break;
    case ModelLifecycleState::Error:
      state.load_state = hardware::LocalModelLoadState::Error;
      state.load_error = status.message;
      break;
  }
  state.execution_plan = papinian_execution_decision().plan;
  {
    auto &benchmark_state = papinian_resource_benchmark_state();
    const std::lock_guard<std::mutex> lock(benchmark_state.mutex);
    state.benchmark = benchmark_state.last_comparison;
  }
  return state;
}

} // namespace

const protected_api::papinian::PapinianGuard &production_guard() {
  return *production_papinian_resources().guard;
}

ModelWarmupTracker &papinian_warmup_tracker() {
  static ModelWarmupTracker tracker([] {
    // Forces VerifiedGliner2Runtime::engine() through the exact same call
    // path a real request uses, rather than adding a separate load-only
    // method to PapinianGuard/VerifiedGliner2Runtime. Standard mode with
    // non-empty text is required: Mode::Off or empty text short-circuits
    // before ever touching the semantic model (see PapinianGuard::protect,
    // guard.cpp).
    protected_api::Policy warm_up_policy;
    warm_up_policy.mode = protected_api::Mode::Standard;

    // Real, measured evidence for this exact warm-up - wall-clock time from
    // its first step to its first finished protection pass, plus this
    // process's actual peak resident memory immediately afterward - compared
    // against what the last finished warm-up of this model version on this
    // backend recorded. Recorded only when the warm-up returns, so that
    // record is also what the next loading status reports as expected.
    std::string backend_key;
    const auto benchmark = hardware::run_resource_benchmark([&] {
      // Decide (and, if it means CPU, apply via FORCE_CPU) the execution
      // backend before ever touching the semantic model - see
      // papinian_execution_decision(). A platform with no viable backend at
      // all fails fast here with a clear reason instead of a confusing
      // lower-level error from deep inside VerifiedGliner2Runtime.
      const auto &decision = papinian_execution_decision();
      if (decision.unavailable_reason) {
        throw std::runtime_error(*decision.unavailable_reason);
      }
      backend_key = hardware::benchmark_key(
          local_models::papinian_model_id(), local_models::papinian_package_version(),
          decision.plan ? std::string(hardware::to_string(decision.plan->backend)) : std::string("unknown"));
      const auto recorded = hardware::recorded_benchmark(papinian_benchmark_baseline_path(), backend_key);
      {
        auto &benchmark_state = papinian_resource_benchmark_state();
        const std::lock_guard<std::mutex> lock(benchmark_state.mutex);
        benchmark_state.expected_warm_up_ms =
            recorded ? std::optional<double>(recorded->warm_up_ms) : std::nullopt;
      }

      production_guard().protect("Papinian warm-up.", warm_up_policy, generate_request_namespace());
    });
    const auto comparison = hardware::record_and_compare_benchmark(
        papinian_benchmark_baseline_path(), backend_key, benchmark);

    auto &benchmark_state = papinian_resource_benchmark_state();
    const std::lock_guard<std::mutex> lock(benchmark_state.mutex);
    benchmark_state.last_comparison = comparison;
  });
  return tracker;
}

std::optional<std::uint64_t> papinian_expected_warm_up_ms() {
  auto &benchmark_state = papinian_resource_benchmark_state();
  const std::lock_guard<std::mutex> lock(benchmark_state.mutex);
  if (!benchmark_state.expected_warm_up_ms) return std::nullopt;
  return static_cast<std::uint64_t>(*benchmark_state.expected_warm_up_ms + 0.5);
}

void prepare_papinian_model_at_launch() {
  auto &packages = local_models::production_model_package_manager();
  const auto papinian = packages.manifest(local_models::papinian_model_id());
  std::error_code error;
  const bool isInstalled = papinian && std::filesystem::is_directory(papinian->installed_path, error) && !error;
  if (isInstalled == false) {
    // Nothing to load yet. The package is fetched first, by itself only once
    // onboarding is complete with protection on (D-198), and its own status
    // shows the download meanwhile, or that it waits to be asked for.
    packages.sync_required_models();
    papinian_warmup_tracker().run_once();
    return;
  }

  // The warm-up verifies the installed package itself (before and after
  // opening it), so readiness does not first wait for the package sync to
  // verify the same bytes. The sync still runs after it, and repairs a
  // damaged package, which is then loaded again.
  papinian_warmup_tracker().run_once();
  packages.sync_required_models();
  if (papinian_warmup_tracker().status().state == ModelLifecycleState::Error &&
      packages.status(local_models::papinian_model_id()).state == local_models::PackageSyncState::Installed) {
    load_papinian_model();
  }
}

std::optional<hardware::BenchmarkComparison> papinian_last_resource_benchmark() {
  auto &benchmark_state = papinian_resource_benchmark_state();
  const std::lock_guard<std::mutex> lock(benchmark_state.mutex);
  return benchmark_state.last_comparison;
}

void load_papinian_model() {
  const auto state = papinian_warmup_tracker().status().state;
  if (state == ModelLifecycleState::Ready || state == ModelLifecycleState::Loading) return;
  // A failed pre-download attempt is intentionally sticky inside the verified
  // runtime. Clear it only at this explicit package-sync/load boundary.
  production_papinian_resources().runtime->unload();
  papinian_warmup_tracker().retry();
}

void unload_papinian_model() {
  papinian_warmup_tracker().mark_unloaded();
  production_papinian_resources().runtime->unload();
}

bool is_papinian_model_loading() {
  return papinian_warmup_tracker().status().state == ModelLifecycleState::Loading;
}

void require_papinian_model_ready(const protected_api::Policy &policy) {
  if (policy.mode == protected_api::Mode::Off) return;
  auto status = papinian_warmup_tracker().status();
  if (status.state == ModelLifecycleState::Ready) return;
  // Loading is not a failure - it is the normal state for the first seconds
  // after launch, while NativeBridge's own background warm-up is still
  // running. Refusing here turned that window into a hard, confusing refusal
  // ("Open Models to prepare protection") for a model that was in fact fine
  // and merely still loading, and it did so for EVERY protected entry point:
  // a real run of matter.runConsilium/matter.synthesizeConsilium failed this
  // way, and the only thing that made it work afterwards was sending an
  // unrelated Chat message first - because SingularityCli.cpp happens to call
  // run_once() before chat.send and nothing else does.
  //
  // run_once() is the SAME canonical warm-up NativeBridge submits at launch:
  // it runs at most once process-wide, blocks until that one warm-up
  // finishes, and never throws. Joining it here means a protected request
  // that arrives mid-warm-up waits for the guard instead of being refused,
  // without any caller having to remember to prepare anything and without
  // weakening the fail-closed rule below - an explicit unload and a genuine
  // load failure still refuse, exactly as before.
  if (status.state == ModelLifecycleState::Loading) {
    papinian_warmup_tracker().run_once();
    status = papinian_warmup_tracker().status();
    if (status.state == ModelLifecycleState::Ready) return;
  }
  if (status.state == ModelLifecycleState::Unloaded) {
    throw providers::TaskError(providers::TaskErrorCode::PrivacyProtectionUnavailable,
        "Papinian NER is unloaded. Load it in Models before making a protected external request.");
  }
  throw providers::TaskError(
      providers::TaskErrorCode::PrivacyProtectionUnavailable,
      status.message.empty()
          ? "Papinian Guard is not ready. Open Models to prepare protection."
          : status.message);
}

hardware::LocalModelRegistry &production_local_model_registry() {
  static hardware::LocalModelRegistry registry = [] {
    hardware::LocalModelRegistry result;
    // Papinian is `required = true`, so release_reclaimable_memory() never
    // actually stops it - this callback exists for architectural
    // completeness (a future non-required module reuses the identical
    // path) and so an explicit user unload still goes through the registry.
    result.register_model(papinian_local_model_descriptor(), papinian_local_model_live_state,
                          [] { unload_papinian_model(); return true; });
		for (const auto &package : local_models::production_model_package_manager().catalog()) {
			if (package.kind != local_models::PackageModelKind::Generative) continue;
			const auto id = package.runtime_model_id;
			result.upsert_model(generative_catalog_descriptor(package),
				[&adapter = local_inference::LocalInferenceService::production_instance(), id] {
					hardware::LocalModelLiveState state;
					state.load_state = adapter.contains(id)
						? hardware::LocalModelLoadState::Unloaded
						: hardware::LocalModelLoadState::NotInstalled;
					return state;
				},
				[&adapter = local_inference::LocalInferenceService::production_instance(), id] {
					return adapter.unload(id);
				});
		}
		local_inference::LocalInferenceService::production_instance().discover_and_register(
			result, production_generative_install_paths());
    return result;
  }();
  return registry;
}

std::map<std::string, std::filesystem::path> production_generative_install_paths() {
	std::map<std::string, std::filesystem::path> result;
	for (const auto &package : local_models::production_model_package_manager().catalog()) {
		if (package.kind == local_models::PackageModelKind::Generative) {
			result.emplace(package.runtime_model_id, package.installed_path);
		}
	}
	// A model file the person chose themselves is known by its own file name and
	// shown where it really lives, never as a copy inside Singularity.
	for (const local_models::LinkedModelFile &linked : local_models::linked_model_files())
		result.emplace(linked.id, linked.file);
	return result;
}

// 12 bytes from the system's secure source (Platform/SecureRandom), as 24
// hexadecimal digits.
std::string generate_request_namespace() {
  return platform::secure_random_hex(12);
}

namespace {

// Everything execute() and preview() must resolve identically before either
// one touches Papinian Guard or a provider - Phase 08 WP8-B's single
// source of truth for request preparation. Factored out of execute() so
// preview() cannot drift into a second, independently-maintained resolution
// path.
struct ResolvedGenerationInputs {
  ModelTarget target;
  std::string api_key;
  std::optional<std::string> base_url;
  protected_api::Policy policy;
  // The selected model's own real, provider-reported context window,
  // carried straight from the already-discovered ModelDescriptor found
  // below - never re-looked-up by provider name downstream. See
  // ContextBudget.hpp's resolve_model_context_window_tokens(), which this
  // feeds as the "discovered" side, falling back to its own catalog only
  // when a provider (direct OpenAI, local) reports no such thing.
  std::optional<std::size_t> model_context_window_tokens;
  // Whether the selected model's own provider-reported capability (see
  // ProviderRegistry.hpp's supports_json_object_response_format()) says it
  // accepts response_format:json_object - never guessed, never assumed from
  // provider or model name.
  bool model_supports_structured_output = false;
  // The selected model's own provider-reported native tool-control
  // capabilities (see ModelClassification.hpp's supports_native_tools/
  // supports_tool_choice) - independent of model_supports_structured_output
  // above and of each other. ResearchToolLoop.hpp consults these (passed
  // straight through, never re-derived from provider/model name) to choose
  // between the native provider tool-call path and the bounded textual
  // fallback, and to decide whether the first Required research turn can
  // force a named tool via tool_choice.
  bool model_supports_native_tools = false;
  bool model_supports_tool_choice = false;
  // Whether a turn may say how long this model thinks
  // (providers::accepts_reasoning_effort()).
  bool model_accepts_reasoning_effort = false;
};

// The selected local model's own real, load-verified context window
// (the context the runtime granted) - see
// LocalModelLiveState::effective_context_tokens' own comment: only ever set
// once this exact model has actually loaded and Singularity reported what it
// really granted after its own hardware-memory-feasibility check, never
// this model's unverified advertised maximum. Absent (this model has not
// loaded yet in this process) returns std::nullopt - never a guess. THE one
// lookup every caller that needs "what is this local model's real context
// window right now" must share, never a second independently-written copy:
// a real Phase 1-28 local-only acceptance run found exactly that split -
// resolve_generation_inputs() (the real generation dispatch, below)
// performed this lookup correctly, while estimate_model_context_window_
// tokens() (every context-budget preflight check upstream of dispatch,
// including ResearchToolLoop's own next-turn continuation-fit check) had
// its own separate, local-provider-excluding branch that never consulted
// it at all - silently falling through to what was then ContextBudget.hpp's
// stale 4096-token local constant (since removed: local inference's own planned/
// granted figures are the only local capability source now) regardless of
// the machine's actual capability, rejecting a
// turn the real dispatch immediately afterward would have accepted.
// local inference's own two figures, one rule (ContextBudget.hpp's
// resolve_local_context_window_tokens()): the context actually granted to
// the current load when Ready, otherwise the feasibility-checked figure Singularity
// planned for this machine without loading, otherwise unknown.
std::optional<std::size_t> live_local_model_context_window_tokens(const std::string &modelId) {
  for (const auto &status : production_local_model_registry().list_status()) {
    if (status.descriptor.id != modelId) continue;
    return research::resolve_local_context_window_tokens(
        status.live.effective_context_tokens, status.live.planned_context_tokens);
  }
  return std::nullopt;
}

// ensureLocalModelLoaded: a real Phase 1-28 local-only acceptance run found a
// genuine, deterministic (not merely racy) ordering defect: on any fresh
// operation where the local model has not already loaded in this process,
// this function's own live_local_model_context_window_tokens() lookup below
// correctly-but-unhelpfully sees LocalModelLoadState::Unloaded (never a
// guess) and returns std::nullopt, and the caller-side fallback (ContextBudget
// .hpp's then-fixed local 4096, today local inference's own pre-load planned figure)
// then governs the ENTIRE multi-turn operation
// - run_research_tool_loop() only ever captures discoveredContextWindowTokens
// ONCE, at the top, never per-turn - even though LocalInferenceService::generate()'s
// own ensure_loaded_locked() finishes the real load moments later and every
// subsequent turn's real generation runs at full capacity regardless. The
// fix is ordering, not a new mechanism: LocalInferenceService::load() (the exact same
// idempotent ensure_loaded_locked() generate() itself would call anyway - an
// already-loaded model returns immediately) simply needs to run BEFORE this
// context-window snapshot is taken, not after. Scoped to true only at
// execute()'s own real-dispatch call site (never require_execution_ready()'s
// cheap readiness probe or preview()'s local no-op branch, neither of which
// should pay a real model-load cost merely to answer a lighter question).
ResolvedGenerationInputs resolve_generation_inputs(
		const InferenceRequest &request, bool ensureLocalModelLoaded = false) {
  const auto settings = platform::load_app_settings();
  const auto &setup = settings.local_node_setup;

  if (request.workspace_id) {
    const auto found = std::find_if(setup.workspaces.begin(), setup.workspaces.end(),
                                    [&](const platform::LocalWorkspace &workspace) {
                                      return workspace.id == *request.workspace_id;
                                    });
    if (found == setup.workspaces.end()) {
      throw std::runtime_error("This workspace is no longer configured. Choose a workspace again.");
    }
    if (!workspaces::directory_available(found->path)) {
      throw std::runtime_error("\"" + found->display_name +
                               "\" is no longer available. Locate it or choose another workspace.");
    }
  }

  ModelTarget target;
  if (request.model_target) {
    target = *request.model_target;
  } else {
    // role_assignments' own "reasoning" entry is the single canonical Node
    // default AI Setup source for Reasoning (Phase 06, AI_SETUP_CONTRACT.md)
    // - setup.default_model is only ever a derived compatibility mirror of
    // it and is never independently consulted here, so changing the
    // canonical Reasoning assignment always changes what a real request
    // actually resolves, with no separate write to default_model required.
    const auto reasoning_target = node_default_reasoning_target(setup);
    if (!reasoning_target) {
      throw providers::TaskError(providers::TaskErrorCode::UnsupportedModel,
                                 "No model is assigned to answer. Choose one in Models.");
    }
    target = *reasoning_target;
  }

  if (target.provider_id == "local") {
    // Ensures installed artifacts have been inspected/registered even when a
    // caller executes directly before opening the Models page.
    (void)production_local_model_registry();
  }
  if (target.provider_id == "local" &&
      local_inference::LocalInferenceService::production_instance().contains(target.model_id)) {
    ResolvedGenerationInputs resolved;
    resolved.target = target;
    resolved.policy.mode = protected_api::Mode::Off;
    // See resolve_generation_inputs()'s own ensureLocalModelLoaded comment:
    // real generation (LocalInferenceService::generate()) would perform this exact
    // idempotent load anyway the moment it runs - doing it here first, only
    // for a real dispatch, means the context-window snapshot immediately
    // below sees this model's own real load-verified capacity instead of
    // the pre-load Unloaded state. A genuine load failure (hardware
    // infeasible, artifact missing) throws here exactly as it would have
    // moments later inside generate() regardless - failing before turn-
    // contract compilation wastes strictly less work, never more.
    if (ensureLocalModelLoaded) local_inference::LocalInferenceService::production_instance().load(target.model_id);
    // The model was just loaded above, so this is local inference's ACTUAL GRANTED
    // context for this exact load - never a pre-load estimate once a real
    // value exists (see live_local_model_context_window_tokens()).
    resolved.model_context_window_tokens = live_local_model_context_window_tokens(target.model_id);
    return resolved;
  }
	if (target.provider_id == "local") {
		throw providers::TaskError(
				providers::TaskErrorCode::UnsupportedModel,
				"The selected local model (\"" + target.model_id +
						"\") is not available on this computer. Open Models to install or retry it.");
	}

  const auto provider = std::find_if(
      setup.connected_providers.begin(), setup.connected_providers.end(),
      [&](const platform::ConnectedProvider &candidate) { return candidate.id == target.provider_id; });
  if (provider == setup.connected_providers.end()) {
    throw providers::TaskError(providers::TaskErrorCode::UnsupportedModel,
                               "This service is not connected. Connect it again in Models.");
  }
  const auto model = std::find_if(provider->models.begin(), provider->models.end(),
                                  [&](const providers::ModelDescriptor &candidate) {
                                    return candidate.id == target.model_id;
                                  });
  if (model == provider->models.end() || !model->text_capable) {
    throw providers::TaskError(
        providers::TaskErrorCode::UnsupportedModel,
        "The selected model (\"" + target.model_id +
            "\") is no longer available from this provider. Choose a model again.");
  }

  ResolvedGenerationInputs resolved;
  resolved.target = target;
  auto credential = platform::get_provider_credential(target.provider_id);
  if (!credential || credential->empty()) {
    throw providers::TaskError(providers::TaskErrorCode::ProviderAuthentication,
                               "Reconnect this service in Models.");
  }
  resolved.api_key = *credential;
  resolved.base_url = provider->base_url;
  if (model->context_window_tokens)
    resolved.model_context_window_tokens = static_cast<std::size_t>(*model->context_window_tokens);
  resolved.model_supports_structured_output = model->supports_structured_output;
  resolved.model_supports_native_tools = model->supports_native_tools;
  resolved.model_supports_tool_choice = model->supports_tool_choice;
  resolved.model_accepts_reasoning_effort = providers::accepts_reasoning_effort(target.provider_id, *model);
  resolved.policy = request.protected_api_policy_override ? *request.protected_api_policy_override
                                                           : setup.protected_api_policy;
  return resolved;
}

} // namespace

void require_execution_ready(const InferenceRequest &request) {
	const auto resolved = resolve_generation_inputs(request);
	if (resolved.target.provider_id != "local") require_papinian_model_ready(resolved.policy);
}

std::string protection_mode_of(const std::string &provider_id, const InferenceRequest &request) {
	if (provider_id == "local") return protected_api::to_string(protected_api::Mode::Off);
	const protected_api::Policy policy = request.protected_api_policy_override ? *request.protected_api_policy_override
		: platform::load_app_settings().local_node_setup.protected_api_policy;
	return protected_api::to_string(policy.mode);
}

std::optional<std::string> research_unavailable_reason(const tools::ToolExecutionContext &context) {
	if (ScopedLocalOnlyWork::is_active()) return std::string(kLocalOnlyWorkResearchUnavailable);
	return research::production_research_unavailable_reason(context);
}

std::optional<std::size_t> estimate_model_context_window_tokens(
		const std::optional<ModelTarget> &target) {
	const auto settings = platform::load_app_settings();
	const auto &setup = settings.local_node_setup;
	ModelTarget resolvedTarget;
	if (target) {
		resolvedTarget = *target;
	} else {
		const auto reasoning_target = node_default_reasoning_target(setup);
		if (!reasoning_target) return std::nullopt;
		resolvedTarget = *reasoning_target;
	}
	std::optional<std::size_t> discovered;
	if (resolvedTarget.provider_id == "local") {
		discovered = live_local_model_context_window_tokens(resolvedTarget.model_id);
	} else {
		const auto provider = std::find_if(setup.connected_providers.begin(), setup.connected_providers.end(),
			[&](const platform::ConnectedProvider &candidate) { return candidate.id == resolvedTarget.provider_id; });
		if (provider != setup.connected_providers.end()) {
			const auto model = std::find_if(provider->models.begin(), provider->models.end(),
				[&](const providers::ModelDescriptor &candidate) { return candidate.id == resolvedTarget.model_id; });
			if (model != provider->models.end() && model->context_window_tokens)
				discovered = static_cast<std::size_t>(*model->context_window_tokens);
		}
	}
	return research::resolve_model_context_window_tokens(
		discovered, resolvedTarget.provider_id, resolvedTarget.model_id);
}

ModelCapability model_capability(const ModelTarget &target) {
	ModelCapability capability;
	if (target.provider_id == "local") {
		const std::optional<std::size_t> window = live_local_model_context_window_tokens(target.model_id);
		if (window.has_value() && *window > 0) {
			capability.context_window_tokens = static_cast<std::uint64_t>(*window);
			capability.max_output_tokens = research::bounded_local_output_tokens(static_cast<std::uint64_t>(*window), *window);
			capability.source = "local";
		}
		return capability;
	}
	const auto settings = platform::load_app_settings();
	const auto &setup = settings.local_node_setup;
	const auto provider = std::find_if(setup.connected_providers.begin(), setup.connected_providers.end(),
		[&](const platform::ConnectedProvider &candidate) { return candidate.id == target.provider_id; });
	if (provider == setup.connected_providers.end()) return capability;
	const auto model = std::find_if(provider->models.begin(), provider->models.end(),
		[&](const providers::ModelDescriptor &candidate) { return candidate.id == target.model_id; });
	if (model == provider->models.end()) return capability;
	capability.context_window_tokens = model->context_window_tokens;
	capability.max_output_tokens = model->max_output_tokens;
	capability.input_price_per_token = model->input_price_per_token;
	capability.output_price_per_token = model->output_price_per_token;
	capability.discovered_at = model->discovered_at;
	if (capability.context_window_tokens || capability.max_output_tokens || capability.input_price_per_token
		|| capability.output_price_per_token) {
		capability.source = "provider_reported";
	}
	return capability;
}

std::optional<std::string> preferred_local_generative_model() {
  // A model that is already loaded costs nothing to ask, and asking another
  // could make the runtime trade one for the other on every call. With none
  // loaded, the smallest installed one: this work is short and frequent, and on
  // a computer whose memory a large model barely fits it is what makes naming a
  // chat a matter of a second rather than ten.
  std::optional<std::string> smallest;
  std::uint64_t smallestBytes = 0;
  for (const hardware::LocalModelStatus &status : production_local_model_registry().list_status()) {
    const auto kind = status.descriptor.execution_profile.architecture_kind;
    const bool writes = kind == hardware::ModelArchitectureKind::DenseTransformer
        || kind == hardware::ModelArchitectureKind::MixtureOfExperts;
    // Installed on disk is not enough: a package the local runtime does not know
    // (its receipt names no model Singularity found) cannot answer.
    if (!writes || !status.installed || status.live.load_state == hardware::LocalModelLoadState::NotInstalled) continue;
    if (status.live.load_state == hardware::LocalModelLoadState::Ready) return status.descriptor.id;
    if (!smallest || (status.disk_bytes > 0 && (smallestBytes == 0 || status.disk_bytes < smallestBytes))) {
      smallest = status.descriptor.id;
      smallestBytes = status.disk_bytes;
    }
  }
  return smallest;
}

InferenceResult execute(const InferenceRequest &request) {
  if (trim(request.prompt).empty()) {
    throw std::runtime_error("Enter a prompt to run.");
  }
	if (request.research_mode == research::ResearchMode::Contradictory) {
		throw std::runtime_error(
			"This request both requires current web research and asks Singularity not to use the internet. Clarify which you want.");
	}
	// Work that runs only on this computer - this thread's ScopedLocalOnlyWork,
	// or the durable record of the operation the request belongs to
	// (Tool/OperationAuthority), whichever thread carries it - runs on a local
	// model, never anything else, and is never offered web research: required,
	// it is refused in words; otherwise the request runs without it.
	bool localOnlyWork = ScopedLocalOnlyWork::is_active();
	if (!localOnlyWork && request.tool_execution_context) {
		const std::optional<tools::OperationAuthority> authority = tools::operation_authority(
			request.tool_execution_context->project_id, request.tool_execution_context->operation_id);
		localOnlyWork = authority && authority->local_only;
	}
	if (localOnlyWork && (!request.model_target || request.model_target->provider_id != "local"))
		throw providers::TaskError(providers::TaskErrorCode::UnsupportedModel,
			"This work runs only on this computer, so it was not sent to a model outside it. Nothing was sent.");
	if (localOnlyWork && request.research_mode == research::ResearchMode::Required)
		throw std::runtime_error(std::string("Web research was not run: ") + kLocalOnlyWorkResearchUnavailable);
	const research::ResearchMode researchMode = localOnlyWork ? research::ResearchMode::Off : request.research_mode;

	// The bounded loop also runs research-free (ResearchMode::Off) whenever the
	// caller validates the final text, so a strict first generation keeps the
	// same single bounded format correction and response repair.
	const bool researchEnabled = request.tool_execution_context.has_value()
		&& (researchMode != research::ResearchMode::Off
			|| static_cast<bool>(request.accepts_research_final_text));
	if (researchMode != research::ResearchMode::Off && request.tool_execution_context.has_value())
		research::ensure_production_research_tools();

	const auto resolved = resolve_generation_inputs(request, /*ensureLocalModelLoaded=*/true);
	// FINAL PHYSICAL ADMISSION (capacity-first law): a request whose first turn
	// cannot fit the executing model's known capacity - local inference's grant for the
	// now-loaded local model, the discovered or catalogued window for a remote
	// one - with the output ceiling it is dispatched with is refused before
	// anything is sent. An operation's TurnContract turns keep their own
	// capacity oracle.
	if (!request.operation_state_compiler) {
		const std::uint64_t requestedOutputTokens = request.maximum_output_tokens.value_or(512);
		const std::uint64_t dispatchedOutputTokens = resolved.target.provider_id == "local"
			? research::bounded_local_output_tokens(requestedOutputTokens, resolved.model_context_window_tokens)
			: requestedOutputTokens;
		if (!research::first_turn_fits_known_capacity(request_turn(request).serialize(),
				research::resolve_model_context_window_tokens(resolved.model_context_window_tokens,
					resolved.target.provider_id, resolved.target.model_id),
				static_cast<std::size_t>(dispatchedOutputTokens)))
			throw providers::TaskError(providers::TaskErrorCode::ContextCapacityExceeded,
				research::kContextCapacityExceededBeforeDispatch);
	}
	const std::string requestNamespace = generate_request_namespace();
	// The task's own table when the caller carries one - its review and
	// corrections then write every value with the symbol the draft used -
	// else one for this call alone.
	protected_api::papinian::OperationProtectedSymbolTable callSymbols;
	protected_api::papinian::OperationProtectedSymbolTable &operationSymbols =
		request.operation_symbols ? *request.operation_symbols : callSymbols;
	if (operationSymbols.operation_id.empty())
		operationSymbols.operation_id = request.tool_execution_context
			? request.tool_execution_context->operation_id : requestNamespace;
	// What the last generation's answer check found (a count): read by the
	// figure check the loop runs on that generation's final text.
	std::size_t lastUntracedFigures = 0;
	// And, under the protected computation protocol, the answer contract's
	// findings on it (DESIGN4 §9.2).
	AnswerContractFindings lastAnswerContract;
	// Figures a tool call carried untraced (a draft's text): no final-text
	// correction reads them, so the lawyer is told beside the answer.
	std::size_t toolCallUntracedFigures = 0;
	auto runGeneration = [&](const ModelVisibleTurn &turn,
			const std::optional<providers::GenerationOutputContract> &outputContract,
			const std::optional<providers::GenerationToolControl> &toolControl) {
		InferenceResult result;
		lastUntracedFigures = 0;
		lastAnswerContract = AnswerContractFindings{};
		// A phase-aware caller (ResearchToolLoop's own operation-turn loop) may
		// carry an override for this exact turn's own output ceiling on the
		// turn itself (see ModelVisibleTurn::max_output_tokens_override's own
		// comment for why the real dispatched request and that loop's
		// predictive fit-check must always agree on the same number). Never
		// used as an excuse to send MORE than the request itself configured;
		// only ever a caller-supplied, phase-appropriate reduction.
		const std::uint64_t effectiveMaxOutputTokens = turn.max_output_tokens_override
			? static_cast<std::uint64_t>(*turn.max_output_tokens_override) : request.maximum_output_tokens.value_or(512);
		// A cloud provider still requires SOME explicit numeric completion
		// ceiling on every request (pure transport mechanics - see
		// ModelVisibleTurn::request_semantic_completion's own comment) even
		// when the semantic contract itself is not token-bounded: resolve the
		// largest safe allowance that provider/model actually documents,
		// rather than this loop's own small phase-derived estimate, so a real
		// semantic completion is never cut short by a number Singularity guessed.
		// Falls back to effectiveMaxOutputTokens unchanged for a model this
		// catalog does not recognize, or for a turn that never requested
		// semantic completion - no behavior change for either case.
		const std::uint64_t effectiveCloudMaxOutputTokens = turn.request_semantic_completion
			? research::known_model_maximum_completion_tokens(
				resolved.target.provider_id, resolved.target.model_id).value_or(effectiveMaxOutputTokens)
			: effectiveMaxOutputTokens;
		if (resolved.target.provider_id == "local") {
    local_inference::GenerativeRequest local_request;
		// A model on this computer reads the values themselves: a history
		// record's local fact anchors (⟪writing|fact:key⟫) are written back as
		// the writing alone - protection never runs here to replace them.
		local_request.prompt = protected_api::papinian::strip_fact_anchor_marks(turn.serialize());
		// ACTUAL GRANTED CAPACITY WINS: the request - and any per-turn override
		// the research loop derived from it - was budgeted before this load,
		// from local inference's planned figure; once the real grant exists the ceiling
		// is re-derived against it with the same half-window rule prepare_
		// task_request() used (ContextBudget.hpp's bounded_local_output_
		// tokens()), so a stale pre-load estimate is never dispatched. A
		// semantic-completion turn is bounded by remaining context instead
		// (bound_output_by_remaining_context below), never by this ceiling.
		local_request.maximum_output_tokens = turn.request_semantic_completion
			? effectiveMaxOutputTokens
			: research::bounded_local_output_tokens(effectiveMaxOutputTokens, resolved.model_context_window_tokens);
    // TOKEN COUNT IS NOT A SEMANTIC COMPLETION CONDITION: a TurnContract-
    // compiled operation turn asks Singularity to run this generation to its own
    // real semantic/schema completion (or genuine physical context
    // exhaustion, reported explicitly - see InferenceResult::
    // physical_context_exhausted), never a caller-guessed output-token
    // ceiling. maximum_output_tokens above is harmlessly still set (ignored
    // by local inference whenever this flag is on) so nothing downstream needs a
    // conditional on which field is meaningful.
    local_request.bound_output_by_remaining_context = turn.request_semantic_completion;
    if (outputContract) {
      local_request.output_constraint = local_inference::GenerativeRequest::OutputConstraint{
          outputContract->schema_name, outputContract->json_schema.dump(), true};
    }
    local_inference::GenerativeResult generation;
    const double localGenerationStarted = platform::trace::now_ms();
    try {
      generation = local_inference::LocalInferenceService::production_instance().generate(
          resolved.target.model_id, local_request);
    } catch (const std::exception &error) {
      // A real production run (this exact machine, see local inference's own
      // llama.cpp HTTP client, HttpClientMacOS.mm) observed the local
      // generation QoS timeout (LocalInferenceService.cpp's own generation_timeout_
      // seconds) not firing early enough to bound a real, evidenced failure
      // class: a request that runs far longer than every comparable turn
      // (llama.cpp's own non-deterministic grammar-stack defect, or a model
      // that never converges to valid structured output) and never returns
      // usable text at all. NSURLSession's own stable, documented message
      // for exceeding timeoutIntervalForRequest is exactly "The request
      // timed out." - matched here, never guessed, and scoped to real
      // TurnContract-compiled operation turns only (request_semantic_
      // completion) so no legacy/non-operation caller's existing timeout
      // behavior changes. Routed through the SAME bounded, single-attempt
      // repair-and-regenerate path physical_context_exhausted already uses
      // just below - a physical transport fact, never a semantic verdict,
      // must never fail the whole operation when one bounded retry (from
      // unchanged canonical state) can recover it.
      // The transport's own typed, locale-independent timeout fact is the
      // authority (platform::HttpTransportError). The English substring is
      // kept ONLY as a fallback for a transport that still throws a plain
      // runtime_error - matching prose alone was silently locale-dependent
      // and never fired on a non-English-locale machine, so a real local
      // timeout there became a hard operation failure instead of taking the
      // bounded repair-and-regenerate path this block exists to reach.
      const auto *transport = dynamic_cast<const platform::HttpTransportError *>(&error);
      const bool requestTimedOut = transport != nullptr
          ? transport->timed_out
          : std::string(error.what()).find("timed out") != std::string::npos;
      if (turn.request_semantic_completion && requestTimedOut) {
        providers::SafeResponseDiagnostics diagnostics;
        diagnostics.issue_code = "generation_timeout";
        diagnostics.normalization_outcome = "physical_qos_timeout";
        diagnostics.repair_class = "incomplete_response";
        throw providers::UnusableGeneration(providers::TaskErrorCode::ResponseIncomplete,
            "The local model did not return a usable response within the generation time limit.",
            providers::UsageMetadata{}, std::string("timeout"), std::move(diagnostics));
      }
      throw;
    }
    // What the local runtime itself measured for this generation, beside the
    // time the whole call took. The difference between the two is the model
    // being loaded or waited for - the part no token speed shows.
    platform::trace::record("local", "generation",
        {{"model", resolved.target.model_id},
         {"promptTokens", generation.prompt_tokens},
         {"evaluatedPromptTokens", generation.evaluated_prompt_tokens},
         {"prefixCacheUsed", generation.prefix_cache_used},
         {"generatedTokens", generation.generated_tokens},
         {"promptMs", generation.prompt_ms},
         {"generationMs", generation.generation_ms},
         {"finish", generation.finish_reason}},
        platform::trace::now_ms() - localGenerationStarted);
    result.text = generation.text;
    result.provider_id = resolved.target.provider_id;
    result.model_id = resolved.target.model_id;
		const auto runtime = local_inference::LocalInferenceService::production_instance().runtime_identity();
		result.execution_kind = "local";
		result.runtime_id = "local";
		result.runtime_version = runtime.version;
		result.runtime_platform = runtime.platform;
		result.runtime_backend = runtime.accelerator_identity.empty()
			? "CPU" : runtime.accelerator_identity;
		result.runtime_model_id =
			local_inference::LocalInferenceService::production_instance()
				.runtime_model_id_for(resolved.target.model_id).value_or(resolved.target.model_id);
		result.runtime_telemetry_json = generation.telemetry_json;
    result.usage.input_tokens = static_cast<long long>(generation.prompt_tokens);
    result.usage.output_tokens = static_cast<long long>(generation.generated_tokens);
    result.usage.total_tokens = static_cast<long long>(generation.prompt_tokens + generation.generated_tokens);
    result.finish_reason = generation.finish_reason;
		result.local_prompt_ms = generation.prompt_ms;
		result.local_generation_ms = generation.generation_ms;
		result.local_evaluated_prompt_tokens = generation.evaluated_prompt_tokens;
		result.local_prefix_cache_used = generation.prefix_cache_used;
		// How fast this machine really is, recorded from the one place every
		// real local generation passes through. The generation ceiling is
		// calculated from this rather than from a benchmark taken on an
		// unthrottled run - see local_generation_timeout_seconds().
		local_model::record_local_generation_throughput(resolved.target.model_id,
			generation.evaluated_prompt_tokens, generation.prompt_ms,
			generation.generated_tokens, generation.generation_ms);
			// finish_reason=="length"/"max_tokens" is no longer converted into an
			// UnusableGeneration/ResponseIncomplete failure here FOR A TURN THAT
			// ACTUALLY REQUESTED SEMANTIC COMPLETION - see the new invariant this
			// migration establishes: a physical output-limit event is a runtime/
			// resource fact, never itself a semantic-completion verdict, for a
			// TurnContract-compiled operation turn. The text flows through
			// normally (exactly as it already did for finish_reason=="stop") and
			// the turn-classification layer (ResearchToolLoop.hpp) - the same
			// layer that already validates every accepted result against the
			// turn's real schema before touching durable state - decides whether
			// it is usable. generation.physical_context_exhausted (only ever true
			// when turn.request_semantic_completion was set) tells that layer
			// whether this specific truncation was a genuine physical boundary.
			// A caller that never requested semantic completion (turn.
			// request_semantic_completion == false: every legacy/non-operation
			// generation, which never reaches ResearchToolLoop's own
			// classification layer at all) keeps today's UNCHANGED behavior - its
			// own configured maximum_output_tokens ceiling being reached is still
			// reported as a genuine ResponseIncomplete failure, exactly as before
			// this migration; nothing here ever silently returns truncated text
			// to a caller with no classification layer able to notice.
			result.physical_context_exhausted = generation.physical_context_exhausted;
			if (!turn.request_semantic_completion
					&& (result.finish_reason == "length" || result.finish_reason == "max_tokens"))
				throw providers::UnusableGeneration(providers::TaskErrorCode::ResponseIncomplete,
					"The model response reached its output limit before it was complete.", result.usage, result.finish_reason);
			return result;
		}
		require_papinian_model_ready(resolved.policy);
		// The caller (ResearchToolLoop today) only ever expresses the LM-owned
		// contract itself; whether it is actually enforced on the wire is
		// decided right here, from the selected model's own real discovered
		// capability - never assumed, never sent to a model that never said
		// it supports structured output.
		const std::optional<providers::GenerationOutputContract> effectiveOutputContract =
			(outputContract.has_value() && resolved.model_supports_structured_output)
				? outputContract : std::nullopt;
		// Same "never send to a model that never said it supports it" gate as
		// effectiveOutputContract above, applied to native tool control -
		// ResearchToolLoop.hpp already decides whether to build a toolControl
		// from this exact resolved.model_supports_native_tools value, so this
		// is defense in depth (never trusting an upstream caller's contract
		// blindly), not the primary decision point.
		const std::optional<providers::GenerationToolControl> effectiveToolControl =
			(toolControl.has_value() && resolved.model_supports_native_tools)
				? toolControl : std::nullopt;
		privacy_actions::ensure_semantic_detection_store();
		// A short think only where this model takes it, and never again for a
		// model that refused it once in this process.
		const std::string effortKey = resolved.target.provider_id + "/" + resolved.target.model_id;
		const bool effortAsked = turn.reasoning_effort != providers::ReasoningEffort::Default;
		const bool effortSent = effortAsked && resolved.model_accepts_reasoning_effort
			&& !refused_reasoning_effort_before(effortKey);
		const auto send = [&](const ModelVisibleTurn &sent,
				const std::optional<providers::GenerationOutputContract> &contract) {
			const auto sendAs = [&](providers::ReasoningEffort effort) {
				ModelVisibleTurn gated = sent;
				gated.reasoning_effort = effort;
				return execute_protected_generation(
					resolved.target.provider_id, resolved.api_key, resolved.target.model_id, gated,
					resolved.base_url, resolved.policy, production_guard(), requestNamespace,
					nullptr, request.project_entity_hints, std::optional<std::uint64_t>(effectiveCloudMaxOutputTokens),
					contract, effectiveToolControl, &operationSymbols);
			};
			if (!effortSent) return sendAs(providers::ReasoningEffort::Default);
			// No thinking, from a model that will not answer without any, is the
			// least it takes.
			providers::ReasoningEffort asked = sent.reasoning_effort;
			if (asked == providers::ReasoningEffort::None && requires_reasoning_before(effortKey))
				asked = providers::ReasoningEffort::Minimal;
			const auto withoutEffort = [&](const providers::TaskError &error) {
				// A model that turns the parameter away is asked again without it,
				// once; the answer is the same call's.
				// Called only from inside a catch: what is rethrown is what was caught.
				if (error.code() != providers::TaskErrorCode::ProviderRejectedRequest
						|| !providers::refuses_reasoning_effort(error.what())) throw;
				remember_reasoning_effort_refusal(effortKey);
				platform::trace::record("inference", "reasoning_effort_refused",
					{{"provider", resolved.target.provider_id}, {"model", resolved.target.model_id}});
				return sendAs(providers::ReasoningEffort::Default);
			};
			try {
				return sendAs(asked);
			} catch (const providers::TaskError &error) {
				if (asked == providers::ReasoningEffort::None && error.code() == providers::TaskErrorCode::ProviderRejectedRequest
						&& providers::requires_reasoning(error.what())) {
					remember_reasoning_required(effortKey);
					platform::trace::record("inference", "reasoning_required",
						{{"provider", resolved.target.provider_id}, {"model", resolved.target.model_id}});
					try {
						return sendAs(providers::ReasoningEffort::Minimal);
					} catch (const providers::TaskError &again) {
						return withoutEffort(again);
					}
				}
				return withoutEffort(error);
			}
		};
		// The contract as a description, in the turn's control plane: Singularity's
		// own protocol state, which Papinian never transforms, so the
		// schema reaches the model exactly as written.
		const auto sendDescribed = [&]() {
			ModelVisibleTurn described = turn;
			described.control.value["responseContract"] = nlohmann::json{
				{"instruction", "Reply with exactly one JSON object and nothing else: no sentence before or after "
					"it and no code fence. It must match responseContract.schema - every property the schema "
					"lists is present, and a property that does not apply to this answer is null."},
				{"schemaName", effectiveOutputContract->schema_name},
				{"schema", effectiveOutputContract->json_schema}};
			auto answered = send(described, std::nullopt);
			answered.text = bare_json_object(answered.text);
			nlohmann::json object = nlohmann::json::parse(answered.text, nullptr, false);
			if (object.is_object()) {
				conform_to_schema(object, effectiveOutputContract->json_schema);
				answered.text = object.dump();
			}
			return answered;
		};
		const std::string modelKey = resolved.target.provider_id + "/" + resolved.target.model_id;
		ProtectedGenerationResult generation;
		if (effectiveOutputContract && refused_grammar_before(modelKey)) {
			generation = sendDescribed();
		} else {
			try {
				generation = send(turn, effectiveOutputContract);
			} catch (const providers::TaskError &error) {
				if (!effectiveOutputContract || error.code() != providers::TaskErrorCode::ProviderRejectedRequest
						|| !refuses_structured_grammar(error.what())) throw;
				remember_grammar_refusal(modelKey);
				platform::trace::record("inference", "structured_output_described",
					{{"provider", resolved.target.provider_id}, {"model", resolved.target.model_id}});
				generation = sendDescribed();
			}
		}
		result.text = generation.text;
		result.tool_calls = generation.tool_calls;
		result.provider_call_skipped = generation.provider_call_skipped;
		result.fact_anchors = generation.fact_anchors;
		result.untraced_figures = generation.untraced_figures.count;
		lastUntracedFigures = result.untraced_figures;
		lastAnswerContract = generation.answer_contract;
		result.answer_contract = generation.answer_contract;
		if (!result.tool_calls.empty()) toolCallUntracedFigures += result.untraced_figures;
  result.provider_id = resolved.target.provider_id;
  result.model_id = resolved.target.model_id;
	result.execution_kind = "external";
	result.runtime_id = resolved.target.provider_id;
	result.runtime_version = "provider_api";
	result.runtime_backend = resolved.target.provider_id;
	result.runtime_model_id = resolved.target.model_id;
  result.protected_api_applied = generation.protected_api_applied;
  result.privacy_metadata = generation.privacy_metadata;
  result.usage = generation.usage;
  result.finish_reason = generation.finish_reason;
  result.physical_context_exhausted = generation.physical_context_exhausted;
		return result;
	};

	// DirectDocumentWork's own "ONE substantive structured-output call, no
	// tool loop" happy path (Document/DirectDocumentWork/DirectDocumentWork.hpp)
	// reaches exactly this branch (no tool_execution_context - never
	// researchEnabled): request.research_final_output_contract, when the
	// caller set it, is honored here as a real strict-JSON-schema response
	// contract - runGeneration()'s own capability gate below still never
	// sends it to a model that never reported supporting it. Every existing
	// caller that leaves this field unset (every one before this change)
	// keeps today's exact behavior: std::nullopt, unchanged.
	if (researchEnabled == false) {
		observability::emit({observability::OperationEventKind::ModelTurnStart,
			"Requesting model generation",
			{{"provider", resolved.target.provider_id}, {"model", resolved.target.model_id},
			 {"purpose", observability::to_string(request.call_purpose)}}, std::nullopt});
		try {
			auto result = runGeneration(request_turn(request),
				request.research_final_output_contract, std::nullopt);
			// Same restored-response forensic capture the bounded loop attaches to
			// every turn (gated by a subscribed sink), so a direct call such as the
			// professional review is never the one response evidence cannot see.
			std::vector<std::pair<std::string, std::string>> details{
				{"provider", resolved.target.provider_id}, {"model", resolved.target.model_id},
				{"purpose", observability::to_string(request.call_purpose)},
				{"textBytes", std::to_string(result.text.size())}};
			if (result.finish_reason) details.emplace_back("finishReason", *result.finish_reason);
			if (result.usage.input_tokens) details.emplace_back("inputTokens", std::to_string(*result.usage.input_tokens));
			if (result.usage.output_tokens) details.emplace_back("outputTokens", std::to_string(*result.usage.output_tokens));
			if (result.usage.total_tokens) details.emplace_back("totalTokens", std::to_string(*result.usage.total_tokens));
			if (result.usage.cost_micros) details.emplace_back("costMicros", std::to_string(*result.usage.cost_micros));
			observability::emit({observability::OperationEventKind::ModelTurnEnd, "Model generation returned",
				std::move(details), observability::has_sink() ? std::optional<std::string>(result.text) : std::nullopt});
			return result;
		} catch (...) {
			observability::emit({observability::OperationEventKind::ModelTurnEnd,
				"Model generation failed",
				{{"provider", resolved.target.provider_id}, {"model", resolved.target.model_id},
				 {"purpose", observability::to_string(request.call_purpose)}, {"outcome", "failed"}}, std::nullopt});
			throw;
		}
	}
	auto researchContext = *request.tool_execution_context;
	if (!researchContext.research_backend)
		researchContext.research_backend = platform::load_app_settings().local_node_setup.research_backend;
	// No tool of an agent that works only on this computer reaches the network,
	// whatever reaches ToolBroker.
	if (localOnlyWork) researchContext.permission.external_network_permitted = false;
	// Every value Papinian protected in this operation's turns so far, read at
	// the moment a research request is minimized (see
	// ToolExecutionContext::operation_protected_values) - with every writing
	// of it the operation met: a person's surname alone and an organisation's
	// short name too (matched there as whole words, as protection matches them).
	researchContext.operation_protected_values = [&operationSymbols]() {
		std::vector<std::string> values;
		for (const protected_api::papinian::RequestMappingEntry &entry : operationSymbols.mapping) {
			for (const std::string &form : entry.every_form()) values.push_back(form);
			values.insert(values.end(), entry.surname_forms.begin(), entry.surname_forms.end());
			values.insert(values.end(), entry.short_forms.begin(), entry.short_forms.end());
		}
		for (const auto &[entityId, aliases] : operationSymbols.project_entity_aliases) {
			(void)entityId;
			values.insert(values.end(), aliases.begin(), aliases.end());
		}
		return values;
	};
	research::CapacityMeasurer operationCapacityMeasurer;
	if (resolved.target.provider_id == "local") {
		const std::string localModelId = resolved.target.model_id;
		operationCapacityMeasurer = [localModelId](const std::string &providerFacingInput) {
			local_inference::GenerativeRequest request;
			request.prompt = providerFacingInput;
			const auto measured = local_inference::LocalInferenceService::production_instance().measure(
				localModelId, request);
			research::CapacityMeasurement result;
			result.fit = measured.fits ? research::CapacityFit::InputFits
				: research::CapacityFit::InputDoesNotFit;
			result.measured_input_tokens = measured.prompt_tokens;
			result.physical_capacity_tokens = measured.context_capacity_tokens;
			result.physical_remaining_tokens = measured.physical_remaining_tokens;
			result.source = "local_exact";
			return result;
		};
	}
	// Local execution owns exactness (DESIGN3 9.6): where this call goes out
	// protected, the model's localFacts are operations over this operation's
	// own symbols, answered here - and its final text is checked for figures
	// no symbol, source or operation gives. A model on this computer reads the
	// values themselves: its questions are answered as before.
	const bool protectedCall = resolved.target.provider_id != "local" && resolved.policy.mode != protected_api::Mode::Off;
	const auto localFactsExecutor = protectedCall
		? over_data_plane(protected_api::papinian::operations::local_facts_executor(production_guard(), operationSymbols,
			resolved.policy))
		: std::function<std::string(const std::vector<context::computed_statements::FactQuestion> &, const std::string &)>();
	// Under the protocol the answer contract checks the final answer (DESIGN4
	// §9.2); elsewhere the figure check does, as before.
	const auto finalTextIssue = protectedCall && (request.research_final_text_issue || !request.operation_model_tool)
		? (request.compute_budget ? with_answer_contract(request.research_final_text_issue, lastAnswerContract)
			: with_figure_check(request.research_final_text_issue, lastUntracedFigures))
		: request.research_final_text_issue;
	// THE PROTECTED COMPUTATION PROTOCOL (DESIGN4 §1): offered where the request
	// carries its budgets and the call really goes out protected. The model's
	// operations - in its final JSON (localFacts.ops) or as lm_compute beside an
	// operation's tools - are worked out here over this operation's own table;
	// the tables of the documents its prompt names are read whole for them, once
	// per operation (DESIGN4 §6.2). Unset budgets: nothing of it is offered.
	protected_api::papinian::compute::LoopBinding computeBinding;
	if (protectedCall && request.compute_budget) {
		if (!operationSymbols.tables_built && request.protected_table_source) {
			// The documents its SOURCE_MATERIAL names, by the handle the prompt
			// gives each ("DOC_3"); the source resolves them.
			std::vector<std::string> handles;
			for (std::size_t at = request.prompt.find("[SOURCE:DOC_"); at != std::string::npos;
					at = request.prompt.find("[SOURCE:DOC_", at + 12)) {
				const std::size_t end = request.prompt.find_first_of(":]", at + 8);
				if (end == std::string::npos) break;
				const std::string handle = request.prompt.substr(at + 8, end - at - 8);
				if (std::find(handles.begin(), handles.end(), handle) == handles.end()) handles.push_back(handle);
			}
			// A table that cannot be read leaves the protocol without it, never
			// the request: its operations over tables are then unknown_symbol.
			try {
				operationSymbols.tables = protected_api::papinian::tables::build_tables(
					request.protected_table_source(handles),
					static_cast<const protected_api::papinian::OperationProtectedSymbolTable &>(operationSymbols),
					protected_api::papinian::compute::table_evidence(request.prompt));
			} catch (const std::exception &) {
				operationSymbols.tables.clear();
			}
			operationSymbols.tables_built = true;
		}
		computeBinding = protected_api::papinian::compute::loop_binding(production_guard(), operationSymbols,
			resolved.policy, *request.compute_budget,
			[](const std::string &prompt) { return without_instruction_sections(prompt).without; });
	}
	// Every search a protected call's model writes is admitted, with or
	// without the protocol's budgets (PRIV-1): a model whose window is too
	// small for the protocol is held to the same rule as any other, judged on
	// what the provider wrote (its reply, kept on the table by each protected
	// call) - and an echo of its own request is written from those bytes too.
	if (protectedCall && !computeBinding.admission) {
		computeBinding.admission = protected_api::papinian::compute::query_admission(production_guard(), operationSymbols,
			[](const std::string &prompt) { return without_instruction_sections(prompt).without; });
		computeBinding.reply = protected_api::papinian::compute::provider_reply(operationSymbols);
	}
	// WHAT A WEB SEARCH OF THIS REQUEST MAY SEND (review 2026-10-11). It is held
	// to the policy a provider's request would be held to: the request's own (a
	// Retry pins the mode its first attempt ran under), else this computer's. A
	// model on this computer read the Matter's own values - its turns are never
	// protected - so under a policy that protects, each search it writes passes
	// Papinian Guard before it leaves (research::local_generation_research_check)
	// and the research tool sends only a search that check passed. Its outbound
	// record carries the protection the call was really held to.
	const protected_api::Policy researchPolicy = protectedCall ? resolved.policy
		: request.protected_api_policy_override ? *request.protected_api_policy_override
		: platform::load_app_settings().local_node_setup.protected_api_policy;
	const research::ResearchCallWiring researchWiring = research::research_call_wiring(protectedCall, researchPolicy,
		production_guard(), request.project_entity_hints, requestNamespace);
	std::function<std::optional<std::string>(const nlohmann::json &, const std::string &)> outboundResearchCheck;
	if (researchWiring.check)
		outboundResearchCheck = [check = researchWiring.check, researchPolicy](const nlohmann::json &arguments,
				const std::string &prompt) {
			require_papinian_model_ready(researchPolicy);
			privacy_actions::ensure_semantic_detection_store();
			return check(arguments, prompt);
		};
	const research::ScopedResearchCallProtection researchCallProtection(researchWiring.protection);
	InferenceResult looped = research::run_research_tool_loop(request.prompt, researchMode,
		researchContext, tools::production_tool_broker(), runGeneration,
		resolved.target.provider_id, resolved.target.model_id, resolved.model_context_window_tokens,
			resolved.model_supports_native_tools, resolved.model_supports_tool_choice,
			request.maximum_output_tokens.value_or(512), std::nullopt,
			request.accepts_research_final_text, request.research_final_text_instruction,
			finalTextIssue, request.research_final_output_contract,
			request.operation_model_tool, request.operation_canonical_tool_id,
			request.operation_tool_handler, request.operation_context_compiler,
			request.operation_checkpoint_handler, request.operation_initial_external_evidence,
			request.operation_initial_progress, request.operation_completion_text,
			request.operation_research_request_model_tool,
			request.operation_research_request_canonical_tool_id,
			request.operation_research_request_handler,
			request.operation_research_completed_handler,
			request.operation_state_compiler,
			request.operation_inspect_model_tool,
			request.operation_inspect_canonical_tool_id,
			request.operation_inspect_handler,
			request.operation_context_compaction_handler,
			request.operation_output_agenda_model_tool,
			request.operation_output_agenda_canonical_tool_id,
			request.operation_output_agenda_handler,
			request.operation_evidence_agenda_model_tool,
			request.operation_evidence_agenda_canonical_tool_id,
			request.operation_evidence_agenda_handler,
			request.operation_map_next_model_tool,
			request.operation_map_next_canonical_tool_id,
			request.operation_map_next_handler,
			request.operation_professional_review_model_tool,
			request.operation_professional_review_canonical_tool_id,
			request.operation_professional_review_handler,
			request.operation_methodology_reveal_model_tool,
			request.operation_methodology_reveal_canonical_tool_id,
			request.operation_methodology_reveal_handler,
			operationCapacityMeasurer, request.call_purpose, request.research_request_from_final_text,
			request.project_evidence_calls_from_final_text, request.maximum_project_evidence_requests,
			request.lawyer_question_from_final_text, request.maximum_lawyer_questions,
			request.local_facts_from_final_text, request.maximum_local_fact_rounds,
			// The facts step only where this call really goes out protected: a
			// provider's model, under a policy that protects - and no longer at
			// all (kLocalFactsStep): the relations Singularity works out travel with
			// the request instead, and the answer turn keeps localFacts.
			context::computed_statements::kLocalFactsStep && resolved.target.provider_id != "local"
					&& resolved.policy.mode != protected_api::Mode::Off
				? request.local_facts_step_contract : std::nullopt,
			request.local_facts_step_instruction, request.local_facts_answered, localFactsExecutor,
			// Carrier N's definition and canonical id are the loop's own
			// (compute_model_tool_definition); the executor answers it.
			std::nullopt, {}, {}, computeBinding.executor,
			request.compute_budget.value_or(protected_api::papinian::compute::ComputeBudgetLimits{}),
			computeBinding.snapshot, computeBinding.resume, computeBinding.localize, computeBinding.admission,
			computeBinding.reply, computeBinding.verbatim, outboundResearchCheck);
	looped.unresolved_figures += toolCallUntracedFigures;
	return looped;
}

PreviewResult preview(const InferenceRequest &request) {
  if (trim(request.prompt).empty()) {
    throw std::runtime_error("Enter a prompt to run.");
  }

  const auto resolved = resolve_generation_inputs(request);
  if (resolved.target.provider_id == "local") {
    PreviewResult result;
    result.provider_id = resolved.target.provider_id;
    result.model_id = resolved.target.model_id;
    result.outbound_text = "";
    result.outbound_request_body = "";
    return result;
  }
  require_papinian_model_ready(resolved.policy);
  // What documents were scored with when they became ready is reused here
  // rather than scored again: the store for this workspace and this model is
  // attached before anything is protected.
  privacy_actions::ensure_semantic_detection_store();

  providers::OutboundCapturingTransport transport;
  try {
    execute_protected_generation(resolved.target.provider_id, resolved.api_key,
                                 resolved.target.model_id, request_turn(request), resolved.base_url,
								 resolved.policy, production_guard(), generate_request_namespace(),
								 &transport, request.project_entity_hints, request.maximum_output_tokens);
  } catch (const OutboundPreviewCaptured &captured) {
    PreviewResult result;
    result.provider_id = resolved.target.provider_id;
    result.model_id = resolved.target.model_id;
    result.protected_api_applied = captured.protected_api_applied;
    result.privacy_metadata = captured.privacy_metadata;
    result.outbound_text = captured.outbound_text;
    result.outbound_request_body = captured.outbound_request_body;
    result.protection = captured.protection;
    return result;
  }
  // execute_protected_generation() only returns normally when it reaches a
  // real provider response - with OutboundCapturingTransport installed
  // above, generate_with_transport() always throws OutboundPayloadCaptured
  // before that can happen (see provider_registry.cpp), which
  // execute_protected_generation() always converts to OutboundPreviewCaptured
  // (see protected_generation.cpp). Reaching this line would mean that
  // guarantee broke - fail loudly rather than silently reporting a preview
  // that captured nothing.
  throw std::logic_error("Outbound preview did not capture a request.");
}

} // namespace singularity::routing
