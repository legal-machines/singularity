#include "LocalModel/UserModelFiles/UserModelFiles.hpp"
#include "Bridge/LocalModelBridge/LocalModelBridge.hpp"

#include "Bridge/LocalModelBridge/LocalModelAssignmentScan.hpp"
#include "Bridge/LocalModelBridge/LocalModelUninstallOrchestrator.hpp"
#include "LocalInference/LocalInferenceService/LocalInferenceService.hpp"
#include "LocalModel/LocalModelRegistry/LocalModelRegistry.hpp"
#include "LocalModel/ModelLifecycle/ModelLifecycle.hpp"
#include "LocalModel/ModelPackageManager/ModelPackageManager.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace singularity::bridge::local_model_bridge {
namespace {

using Json = nlohmann::json;

const char *load_state_string(hardware::LocalModelLoadState state) {
  switch (state) {
    case hardware::LocalModelLoadState::NotInstalled: return "not_installed";
    case hardware::LocalModelLoadState::Loading: return "loading";
    case hardware::LocalModelLoadState::Ready: return "ready";
    case hardware::LocalModelLoadState::Unloaded: return "unloaded";
    case hardware::LocalModelLoadState::Error: return "error";
  }
  return "error";
}

const char *compatibility_string(hardware::CompatibilityLevel level) {
  switch (level) {
    case hardware::CompatibilityLevel::Recommended: return "recommended";
    case hardware::CompatibilityLevel::WillRunSlowly: return "will_run_slowly";
    case hardware::CompatibilityLevel::NotRecommended: return "not_recommended";
    case hardware::CompatibilityLevel::Unsupported: return "unsupported";
  }
  return "not_recommended";
}

Json execution_profile_json(const hardware::ModelExecutionProfile &profile) {
  Json result{{"architectureKind", hardware::to_string(profile.architecture_kind)},
             {"architectureDescription", profile.architecture_description},
             {"precision", profile.precision},
             {"requiresRuntimeState", profile.requires_runtime_state}};
  if (profile.quantization) result["quantization"] = *profile.quantization;
  if (profile.model_state_bytes) result["modelStateBytes"] = *profile.model_state_bytes;
  if (profile.expert_count) result["expertCount"] = *profile.expert_count;
  if (profile.active_experts_per_token) result["activeExpertsPerToken"] = *profile.active_experts_per_token;
  return result;
}

Json load_impact_json(const hardware::LoadImpact &impact) {
  return Json{{"expectedBytes", impact.expected_bytes},
             {"level", compatibility_string(impact.level)},
             {"reason", impact.reason}};
}

Json resource_benchmark_json(const hardware::ResourceBenchmark &benchmark) {
  return Json{{"warmUpMs", benchmark.warm_up_ms}, {"peakResidentMemoryBytes", benchmark.peak_resident_memory_bytes}};
}

Json benchmark_comparison_json(const hardware::BenchmarkComparison &comparison) {
  Json result{{"hasBaseline", comparison.has_baseline},
             {"regressed", comparison.regressed},
             {"current", resource_benchmark_json(comparison.current)}};
  if (comparison.has_baseline) result["baseline"] = resource_benchmark_json(comparison.baseline);
  return result;
}

// One of the seven Beta Model Library card states - Available/Downloading/
// Installed/Preparing/"Ready to use"/"Needs attention"/Unavailable.
struct CardState {
  std::string state;
  std::string detail;
  std::optional<int> progress_percent;
  // True only when the "Needs attention" state can be resolved with the
  // password-authorization flow (localModels.authorize) - never set for a
  // load-state failure, which is a lifecycle concern owned by a later
  // phase.
  bool needs_authorization = false;
  // True only when the "Needs attention" state can be resolved by simply
  // retrying the same package sync (localModels.sync).
  bool can_retry = false;
  // True only when the "Needs attention" state comes from a load-time
  // failure (the package itself installed fine) and can be resolved by
  // retrying the load (localModels.load) - a different native action than
  // can_retry above, so the frontend must never confuse the two.
  bool can_retry_load = false;
};

// Merges this model's package-download truth (when one applies - only
// Papinian NER is package-manager-backed today) with its load truth into
// one card state a Model Library card can render directly, so the frontend
// never re-derives product meaning from raw native enums.
CardState card_state_for(const hardware::LocalModelStatus &status,
                         const std::optional<local_models::PackageSyncStatus> &package) {
  if (package) {
    switch (package->state) {
		case local_models::PackageSyncState::Available:
			break;
      case local_models::PackageSyncState::AuthorizationRequired:
        return {"needs_attention", package->message, std::nullopt, /*needs_authorization=*/true,
               /*can_retry=*/false};
      case local_models::PackageSyncState::Error:
        return {"needs_attention", package->message, std::nullopt, /*needs_authorization=*/false,
               /*can_retry=*/true};
      case local_models::PackageSyncState::CheckingServer:
      case local_models::PackageSyncState::Verifying:
      case local_models::PackageSyncState::Installing:
        return {"preparing", package->message, std::nullopt, false, false};
      case local_models::PackageSyncState::Downloading:
        return {"downloading", package->message, package->progress_percent, false, false};
      case local_models::PackageSyncState::Installed:
        break; // Installed - fall through to this model's load truth below.
    }
  }

  switch (status.live.load_state) {
    case hardware::LocalModelLoadState::NotInstalled:
      return {"available", "", std::nullopt, false, false};
    case hardware::LocalModelLoadState::Loading:
      return {"preparing", "", std::nullopt, false, false};
    case hardware::LocalModelLoadState::Ready:
      return {"ready_to_use", "", std::nullopt, false, false};
    case hardware::LocalModelLoadState::Unloaded:
      return {"installed", "", std::nullopt, false, false};
    case hardware::LocalModelLoadState::Error:
      return {"needs_attention", status.live.load_error, std::nullopt, false, false, /*can_retry_load=*/true};
  }
  return {"unavailable", "", std::nullopt, false, false};
}

// The one way this computer can get the protection model while it has none
// (local_models::papinian_recovery_for says which, and why).
std::string papinian_install_recovery(local_models::PackageSyncState state) {
	auto &manager = local_models::production_model_package_manager();
	return local_models::papinian_recovery_for(state, manager.has_session(),
	                                           local_models::papinian_developer_copy_available());
}

// A warm-up failure said whole: what did not happen, and why when Singularity
// knows more than the guard's own short sentence.
std::string papinian_start_failure(const std::string &reason) {
	const std::string base = "Papinian Guard's protection model is on this computer but did not start";
	if (reason.empty() == true || reason == "Papinian Guard's local model isn't available right now.") {
		return base + ", so requests that need protection wait. Start it again; if it still does not start, restart Singularity.";
	}
	return base + ": " + reason;
}

Json serialize(const hardware::LocalModelStatus &status) {
	auto &manager = local_models::production_model_package_manager();
	const auto manifest = manager.manifest(status.descriptor.id);
	const std::optional<local_models::PackageSyncStatus> package =
		manifest ? std::optional<local_models::PackageSyncStatus>(manager.status(status.descriptor.id))
		         : std::nullopt;
  const auto card = card_state_for(status, package);
	const auto download_bytes = manifest ? manifest->artifact_bytes : status.disk_bytes;
	// A package Singularity installs itself is on this computer only once the
	// package says so. The registry's own answer counts a protection model whose
	// warm-up failed for want of a package as present, which the Library then
	// showed as "installed here" on a computer that has none.
	const bool isInstalled = manifest && manifest->kind == local_models::PackageModelKind::EntityProcessing
		? package->state == local_models::PackageSyncState::Installed
		: status.installed;

  Json result{{"id", status.descriptor.id},
              {"displayName", status.descriptor.display_name},
              {"purpose", status.descriptor.purpose},
              {"role", status.descriptor.role},
              {"family", status.descriptor.family},
              {"developer", status.descriptor.developer},
              {"version", status.descriptor.version},
              {"required", status.descriptor.required},
              {"installed", isInstalled},
              {"diskBytes", status.disk_bytes},
              {"downloadBytes", download_bytes},
              {"loadState", load_state_string(status.live.load_state)},
              {"compatibilityLevel", compatibility_string(status.compatibility.level)},
              {"compatibilityReason", status.compatibility.reason},
              {"cardState", card.state},
              {"needsAuthorization", card.needs_authorization},
              {"canRetry", card.can_retry},
              {"canRetryLoad", card.can_retry_load},
              // The protection model can be installed on this computer now, from
              // a Debug build's developer copy (localModels.sync papinian-ner).
              {"canInstallHere", manifest && manifest->kind == local_models::PackageModelKind::EntityProcessing
                                     && isInstalled == false
                                     && papinian_install_recovery(package->state) == "install_here"},
              {"executionProfile", execution_profile_json(status.descriptor.execution_profile)},
              {"loadImpact", load_impact_json(status.load_impact)}};
  if (!card.detail.empty()) result["cardStateDetail"] = card.detail;
  if (card.progress_percent) result["downloadProgressPercent"] = *card.progress_percent;
  if (package && package->done_bytes && package->total_bytes && card.state != "needs_attention") {
    result["downloadedBytes"] = *package->done_bytes;
  }
  if (status.live.load_state == hardware::LocalModelLoadState::Error) result["loadError"] = status.live.load_error;
  if (status.live.execution_plan) {
    result["executionBackend"] = hardware::to_string(status.live.execution_plan->backend);
    result["executionReason"] = status.live.execution_plan->reason;
  }
  if (status.live.benchmark) result["benchmark"] = benchmark_comparison_json(*status.live.benchmark);
	if (manifest) {
		result["package"] = Json{{"id", manifest->id},
		                         {"runtimeModelId", manifest->runtime_model_id},
		                         {"kind", manifest->kind == local_models::PackageModelKind::Generative
		                                      ? "generative" : "entity_processing"},
		                         {"capabilities", manifest->capabilities},
		                         {"format", manifest->format},
		                         {"sourceRepository", manifest->source_repository},
		                         {"sourceRevision", manifest->source_revision},
		                         {"artifactFilename", manifest->artifact_filename},
		                         {"artifactBytes", manifest->artifact_bytes},
		                         {"artifactSha256", manifest->artifact_sha256},
		                         {"license", manifest->license},
		                         {"licenseUrl", manifest->license_url}};
	}
  return result;
}

const char *warmup_phase_string(routing::ModelWarmupPhase phase) {
  switch (phase) {
    case routing::ModelWarmupPhase::Starting: return "starting";
    case routing::ModelWarmupPhase::LoadingModel: return "loading_model";
    case routing::ModelWarmupPhase::Preparing: return "preparing";
  }
  return "starting";
}

// While Papinian loads: the warm-up's real current step, how long it has run,
// and how long the last finished one took on this computer (null the first
// time this model version loads here).
Json papinian_loading_json(const routing::ModelLifecycleStatus &status) {
  Json result{{"status", "loading"},
             {"phase", warmup_phase_string(status.phase)},
             {"elapsedMs", status.elapsed_ms},
             {"expectedMs", nullptr}};
  const auto expected = routing::papinian_expected_warm_up_ms();
  if (expected) result["expectedMs"] = *expected;
  return result;
}

} // namespace

nlohmann::json use_existing_file(const std::optional<std::string> &selected_path) {
	if (!selected_path.has_value()) return list(Json::object());
	const auto linked = local_models::link_model_file(std::filesystem::path(*selected_path));
	(void)linked;
	// The new model has to be discovered before the Model Library can show it.
	auto &registry = routing::production_local_model_registry();
	local_inference::LocalInferenceService::production_instance().rediscover_after_package_change(
		registry, routing::production_generative_install_paths());
	return list(Json::object());
}

nlohmann::json forget_existing_file(const nlohmann::json &params) {
	if (!params.contains("modelId") || !params.at("modelId").is_string())
		throw std::runtime_error("This local model is not available.");
	const auto model_id = params.at("modelId").get<std::string>();
	auto &registry = routing::production_local_model_registry();
	if (!local_inference::LocalInferenceService::production_instance().unload(model_id))
		throw std::runtime_error("This model is answering right now. Try again once it finishes.");
	local_models::forget_model_file(model_id);
	local_inference::LocalInferenceService::production_instance().rediscover_after_package_change(
		registry, routing::production_generative_install_paths());
	registry.unregister_model(model_id);
	return list(Json::object());
}

nlohmann::json list(const nlohmann::json &) {
  Json result = Json::array();
  for (const auto &status : routing::production_local_model_registry().list_status()) result.push_back(serialize(status));
  return result;
}

nlohmann::json runtime_state(const nlohmann::json &) {
	try {
		// Initializing the production registry performs the same discovery
		// routing uses. This is an observation endpoint, never a second model
		// catalogue or a frontend-owned availability calculation.
		(void)routing::production_local_model_registry();
		auto &service = local_inference::LocalInferenceService::production_instance();
		const auto identity = service.runtime_identity();
		return Json{{"available", identity.available},
		            {"runtimeId", "local"},
		            {"runtimeVersion", identity.version},
		            {"buildType", identity.build_type},
		            {"platform", identity.platform},
		            {"backend", identity.accelerator_identity.empty() ? "CPU" : identity.accelerator_identity},
		            {"systemMemoryBytes", identity.system_memory_bytes},
		            {"models", service.known_model_ids()},
		            {"error", identity.available ? Json(nullptr)
		                                         : Json("Local models cannot run on this computer.")}};
	} catch (const std::exception &error) {
		return Json{{"available", false},
		            {"runtimeId", "local"},
		            {"runtimeVersion", ""},
		            {"buildType", ""},
		            {"platform", ""},
		            {"backend", ""},
		            {"systemMemoryBytes", 0},
		            {"models", Json::array()},
		            {"error", error.what()}};
	}
}

// What getModelStatus says while the protection model's package is not yet
// usable: the package's own state and plain words, how much of it has arrived
// while it moves, and the one thing a person can do about it - "sign_in",
// "retry_download" or "retry_load" - or null when there is nothing to do but
// wait. The frontend offers exactly that action and never guesses another.
Json papinian_package_json(const char *status, const local_models::PackageSyncStatus &package,
                           const Json &recovery) {
  Json result{{"status", status}, {"message", package.message}, {"recovery", recovery}};
  if (package.done_bytes && package.total_bytes) {
    result["doneBytes"] = *package.done_bytes;
    result["totalBytes"] = *package.total_bytes;
  }
  if (package.progress_percent) result["progressPercent"] = *package.progress_percent;
  return result;
}

nlohmann::json papinian_status(const nlohmann::json &) {
	auto &manager = local_models::production_model_package_manager();
	const auto package = manager.status(local_models::papinian_model_id());
	const std::string recovery = papinian_install_recovery(package.state);
  switch (package.state) {
	case local_models::PackageSyncState::Available: {
		local_models::PackageSyncStatus shown = package;
		shown.message = recovery == "install_here"
			? "Papinian Guard's protection model is not on this computer yet. It can be installed from the copy this build of Singularity carries."
			: recovery == "sign_in"
				? "Papinian Guard's protection model is not on this computer yet. Signing in to Legal Machines downloads it."
				: "Papinian Guard's protection model is not on this computer yet.";
		return papinian_package_json("available", shown, recovery);
	}
    case local_models::PackageSyncState::CheckingServer:
      return papinian_package_json("checking_server", package, nullptr);
    case local_models::PackageSyncState::AuthorizationRequired: {
      local_models::PackageSyncStatus shown = package;
      if (recovery == "install_here") {
        shown.message = "Papinian Guard's protection model is not on this computer yet. It can be installed from the copy this build of Singularity carries.";
      } else if (recovery == "sign_in") {
        shown.message = "Papinian Guard's protection model is not on this computer yet. Signing in to Legal Machines downloads it.";
      }
      return papinian_package_json("authorization_required", shown, recovery);
    }
    case local_models::PackageSyncState::Downloading:
      return papinian_package_json("downloading", package, nullptr);
    case local_models::PackageSyncState::Verifying:
      return papinian_package_json("verifying", package, nullptr);
    case local_models::PackageSyncState::Installing:
      return papinian_package_json("installing", package, nullptr);
    case local_models::PackageSyncState::Error:
      return papinian_package_json("error", package, recovery);
    case local_models::PackageSyncState::Installed: break;
  }
  const auto status = routing::papinian_warmup_tracker().status();
  switch (status.state) {
    case routing::ModelLifecycleState::Loading: return papinian_loading_json(status);
    case routing::ModelLifecycleState::Ready: return Json{{"status", "ready"}, {"recovery", nullptr}};
    case routing::ModelLifecycleState::Unloaded:
      return Json{{"status", "unloaded"},
                  {"message", "Papinian Guard's protection model is on this computer but not running."},
                  {"recovery", "retry_load"}};
    case routing::ModelLifecycleState::Error:
      return Json{{"status", "error"}, {"message", papinian_start_failure(status.message)}, {"recovery", "retry_load"}};
  }
  return Json{{"status", "error"}, {"message", "Unknown model status"}, {"recovery", "retry_load"}};
}

nlohmann::json authorize(const nlohmann::json &params) {
  // A password alone, with no sign-in code: a seat that needs one is told to
  // sign in again (account.code_required), where the code is asked for.
  local_models::production_model_package_manager().authorize_and_sync(
      params.at("password").get<std::string>(), "");
  routing::papinian_warmup_tracker().run_once();
  routing::load_papinian_model();
  return papinian_status(Json::object());
}

nlohmann::json sync(const nlohmann::json &params) {
  // A card's Install/Try again action must target that card's own model,
  // not silently trigger whatever the required-model set happens to sync
  // today - the same defensive shape load()/unload() already use below.
	if (!params.contains("modelId") || !params.at("modelId").is_string()) {
    throw std::runtime_error("This local model is not available.");
  }
	const auto model_id = params.at("modelId").get<std::string>();
	auto &manager = local_models::production_model_package_manager();
	const auto package = manager.manifest(model_id);
	if (!package) throw std::runtime_error("This local model is not available.");
	std::optional<std::filesystem::path> existing_artifact;
	if (params.contains("existingArtifactPath")) {
		if (!params.at("existingArtifactPath").is_string()) {
			throw std::runtime_error("The downloaded model file is invalid.");
		}
		existing_artifact = params.at("existingArtifactPath").get<std::string>();
	}
	manager.install(model_id, existing_artifact);
	if (package->kind == local_models::PackageModelKind::EntityProcessing) {
		routing::papinian_warmup_tracker().run_once();
		routing::load_papinian_model();
		return papinian_status(Json::object());
	}
	auto &registry = routing::production_local_model_registry();
	local_inference::LocalInferenceService::production_instance().rediscover_after_package_change(
		registry, routing::production_generative_install_paths());
	if (!local_inference::LocalInferenceService::production_instance().contains(package->runtime_model_id)) {
		throw std::runtime_error("Singularity did not find the installed model.");
	}
	return list(Json::object());
}

nlohmann::json load(const nlohmann::json &params) {
  if (!params.contains("modelId") || !params.at("modelId").is_string()) {
    throw std::runtime_error("This local model is not available.");
  }
	(void)routing::production_local_model_registry();
  const auto model_id = params.at("modelId").get<std::string>();
  if (local_inference::LocalInferenceService::production_instance().contains(model_id)) {
    local_inference::LocalInferenceService::production_instance().load(model_id);
    return list(Json::object());
  }
  if (model_id != "papinian-ner") throw std::runtime_error("This local model is not available.");
	const auto package = local_models::production_model_package_manager().status(
		local_models::papinian_model_id());
  if (package.state != local_models::PackageSyncState::Installed) {
    throw std::runtime_error("Install Papinian NER before loading it.");
  }
  routing::load_papinian_model();
  return list(Json::object());
}

// The real assignment scan for `model_id`, bound to real Node settings and
// real Matter/.singularity-workspace storage - see LocalModelAssignmentScan.hpp's own
// contract for exactly what this does and does not consider verified.
local_model_bridge::AssignmentScanResult assignment_scan_for(const std::string& model_id) {
	const routing::ModelTarget target{"local", model_id};
	platform::LocalNodeSetup setup;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		setup = platform::load_app_settings().local_node_setup;
	}
	return local_model_bridge::gather_assignment_references(
		setup, target,
		[](const std::filesystem::path& path) { return matters::load_matters(path); },
		[](const std::filesystem::path& path) { return workspaces::directory_available(path); });
}

// Local storage/Manage's "Remove from this computer" command - deletes the
// installed artifact from disk. Distinct from unload() above, which only
// stops a loaded model from occupying memory and leaves its package on
// disk; distinct from AI Setup, which this never rewrites. Gathers real
// Node/Project/Chat/registry truth here and defers the actual
// decision/sequencing to LocalModelUninstallOrchestrator.hpp's
// uninstall_local_model(), which is what
// LocalModelUninstallOrchestrator.test.cpp exercises directly against a
// real hardware::LocalModelRegistry and real filesystem operations.
nlohmann::json remove(const nlohmann::json& params) {
	if (!params.contains("modelId") || !params.at("modelId").is_string()) {
		throw std::runtime_error("This local model is not available.");
	}
	const auto model_id = params.at("modelId").get<std::string>();
	auto& registry = routing::production_local_model_registry();
	const auto statuses = registry.list_status();
	const auto found = std::find_if(statuses.begin(), statuses.end(),
		[&](const hardware::LocalModelStatus& status) { return status.descriptor.id == model_id; });
	if (found == statuses.end()) throw std::runtime_error("This local model is not available.");

	const auto package = local_models::production_model_package_manager().manifest(model_id);
	const auto disposition = package && !package->required
		? local_model_bridge::UninstallDisposition::PreserveIdentity
		: local_model_bridge::UninstallDisposition::Unsupported;

	local_model_bridge::uninstall_local_model(*found, disposition, assignment_scan_for(model_id),
		[&]() {
			if (!local_inference::LocalInferenceService::production_instance().unload(model_id))
				throw std::runtime_error("This model is answering right now. Try again once it finishes.");
			local_models::production_model_package_manager().remove(model_id);
			local_inference::LocalInferenceService::production_instance().rediscover_after_package_change(
				registry, routing::production_generative_install_paths());
		},
		[&]() { registry.unregister_model(model_id); });
	return list(Json::object());
}

nlohmann::json unload(const nlohmann::json &params) {
  if (!params.contains("modelId") || !params.at("modelId").is_string()) {
    throw std::runtime_error("This local model is not available.");
  }
	(void)routing::production_local_model_registry();
  const auto model_id = params.at("modelId").get<std::string>();
  if (local_inference::LocalInferenceService::production_instance().contains(model_id)) {
    if (!local_inference::LocalInferenceService::production_instance().unload(model_id))
      throw std::runtime_error("This model is answering right now. Try again once it finishes.");
    return list(Json::object());
  }
  if (model_id != "papinian-ner") throw std::runtime_error("This local model is not available.");
  routing::unload_papinian_model();
  return list(Json::object());
}

} // namespace singularity::bridge::local_model_bridge
