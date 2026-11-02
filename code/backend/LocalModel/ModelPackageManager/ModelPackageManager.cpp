#include "LocalModel/ModelPackageManager/ModelPackageManager.hpp"

#include "LocalModel/DeveloperModelSource/DeveloperModelSource.hpp"
#include "LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.hpp"
#include "Node/LegalMachinesAccount/LegalMachinesAccount.hpp"
#include "Platform/AppPaths/AppPaths.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Node/LegalMachinesConnection/LegalMachinesConnection.hpp"
#include "Platform/HttpClient/HttpClient.hpp"
#include "Platform/Sha256/Sha256.hpp"
#include "Provider/CredentialStore/CredentialStore.hpp"

#include <nlohmann/json.hpp>
#include <zip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>

namespace singularity::local_models {
namespace {

constexpr const char *kApiBase = node::kLegalMachinesApiBase;
constexpr const char *kSessionCredential = "singularity-session";
constexpr const char *kPapinianId = "papinian-ner";
constexpr const char *kPapinianVersion = "1.0.0";
constexpr const char *kArchiveSha256 = "0f70a2a3eb1abac455e6a1cdba6e686e27d138a55eec05f0a13edcbded8ef91d";
constexpr std::uint64_t kArchiveBytes = 631507747;
constexpr const char *kQwenContextId = "qwen3-30b-a3b-q4-k-m";
constexpr const char *kQwenContextRevision = "5e715e4820c9800095ca3e0b217139ceb2ba38f0";
constexpr const char *kQwenContextFilename = "Qwen3-30B-A3B-Q4_K_M.gguf";
constexpr const char *kQwenContextSha256 = "642a1eda167db5f55de673cbd19fd8b4aac69c766d01bdbeaf5820f457c7790d";
constexpr std::uint64_t kQwenContextBytes = 18556686240ULL;
// A whole model package may take hours on a slow connection; a stalled one
// still fails within a minute (platform::https_download_file).
constexpr int kPackageDownloadTimeoutSeconds = 4 * 60 * 60;

// What a person reads about the protection model's download, in plain words.
constexpr const char *kSignInToDownload =
    "Sign in to Legal Machines to download Papinian Guard's protection model.";
constexpr const char *kSignInAgainToDownload =
    "Your Legal Machines sign-in has ended. Sign in again to download Papinian Guard's protection model.";
constexpr const char *kLegalMachinesUnreachable =
    "Singularity could not reach Legal Machines. Check the internet connection, then try again.";
constexpr const char *kChooseExistingFile =
    "Singularity does not download large models in this version. If this model is already on this computer, "
    "choose its file with \u201cUse a file I already have\u201d.";

std::filesystem::path models_root() {
  return platform::application_data_directory() / "Models";
}

std::filesystem::path papinian_root() {
  return models_root() / kPapinianId / kPapinianVersion;
}

std::string random_suffix();

std::filesystem::path generative_root() {
	return models_root() / "generative";
}

std::filesystem::path package_cache_root() {
	return models_root() / "package-cache";
}

std::filesystem::path receipt_path(const std::string &model_id) {
	return models_root() / "package-receipts" / (model_id + ".json");
}

std::vector<ModelPackageManifest> catalog_manifests() {
	return {
		{kPapinianId, kPapinianId, "Papinian NER", PackageModelKind::EntityProcessing,
		 {"entity_extraction", "outbound_protection"}, "verified-gliner2-package",
		 "legal-machines/model-catalog", kPapinianVersion,
		 std::string(kApiBase) + "/local-models/" + kPapinianId + "/" + kPapinianVersion,
		 "papinian-ner-1.0.0.zip", kArchiveBytes, kArchiveSha256, "proprietary",
		 "https://legalmachines.org/terms", papinian_root(), true},
		{kQwenContextId, kQwenContextId, "Qwen3 30B A3B Q4_K_M",
		 PackageModelKind::Generative, {"text_generation", "structured_context"}, "GGUF",
		 "ggml-org/Qwen3-30B-A3B-GGUF", kQwenContextRevision,
		 std::string("https://huggingface.co/ggml-org/Qwen3-30B-A3B-GGUF/resolve/") +
		     kQwenContextRevision + "/" + kQwenContextFilename,
		 kQwenContextFilename, kQwenContextBytes, kQwenContextSha256, "Apache-2.0",
		 "https://huggingface.co/Qwen/Qwen3-30B-A3B/blob/main/LICENSE",
		 generative_root() / kQwenContextFilename, false},
	};
}

const ModelPackageManifest &require_manifest(const std::string &model_id) {
	static const auto manifests = catalog_manifests();
	const auto found = std::find_if(manifests.begin(), manifests.end(),
		[&](const ModelPackageManifest &candidate) {
			return candidate.id == model_id || candidate.runtime_model_id == model_id;
		});
	if (found == manifests.end()) throw std::runtime_error("This local model is not available.");
	return *found;
}

void verify_file(const std::filesystem::path &path, const ModelPackageManifest &manifest) {
	std::error_code error;
	if (!std::filesystem::is_regular_file(path, error) || error ||
	    std::filesystem::file_size(path, error) != manifest.artifact_bytes || error ||
	    platform::sha256_hex_digest_of_file(path) != manifest.artifact_sha256) {
		throw std::runtime_error("The model artifact failed integrity verification.");
	}
}

void write_receipt(const ModelPackageManifest &manifest) {
	const auto destination = receipt_path(manifest.id);
	std::filesystem::create_directories(destination.parent_path());
	const auto temporary = destination.string() + ".tmp-" + random_suffix();
	std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
	output << nlohmann::json{{"schemaVersion", 1},
	                         {"modelId", manifest.id},
	                         {"runtimeModelId", manifest.runtime_model_id},
	                         {"sourceRepository", manifest.source_repository},
	                         {"sourceRevision", manifest.source_revision},
	                         {"artifactFilename", manifest.artifact_filename},
	                         {"artifactBytes", manifest.artifact_bytes},
	                         {"artifactSha256", manifest.artifact_sha256},
	                         {"installedPath", manifest.installed_path.string()}}
	              .dump(2);
	output.close();
	if (!output) {
		std::filesystem::remove(temporary);
		throw std::runtime_error("The model installation receipt could not be written.");
	}
	std::error_code ignored;
	std::filesystem::remove(destination, ignored);
	std::filesystem::rename(temporary, destination);
}

bool receipt_matches(const ModelPackageManifest &manifest) {
	try {
		std::ifstream input(receipt_path(manifest.id));
		const auto receipt = nlohmann::json::parse(input);
		return receipt.at("modelId") == manifest.id &&
		       receipt.at("runtimeModelId") == manifest.runtime_model_id &&
		       receipt.at("sourceRevision") == manifest.source_revision &&
		       receipt.at("artifactBytes") == manifest.artifact_bytes &&
		       receipt.at("artifactSha256") == manifest.artifact_sha256 &&
		       receipt.at("installedPath") == manifest.installed_path.string();
	} catch (...) {
		return false;
	}
}

std::string random_suffix() {
  std::random_device seed;
  return std::to_string((static_cast<std::uint64_t>(seed()) << 32) | seed());
}

std::vector<platform::HttpHeader> authorized_headers() {
  const auto settings = platform::load_app_settings();
  const auto token = platform::get_provider_credential(kSessionCredential);
  if (!settings.account || !token || token->empty()) return {};
  return {{"Authorization", "Bearer " + *token},
          {"X-LM-Organisation", settings.account->organisation_id},
          {"X-LM-Seat", settings.account->seat_id},
          {"X-LM-Node", model_catalog_node_id()}};
}

// Legal Machines turned this computer's session away (401/403): it is dropped,
// so no "Try again" sends the same rejected session, and the recovery offered
// becomes signing in again (papinian_recovery_for). Failing to drop it leaves
// the AuthorizationRequired state, which already offers only signing in.
void forget_rejected_session() {
  try {
    platform::delete_provider_credential(kSessionCredential);
  } catch (...) {
  }
}

void extract_package(const std::filesystem::path &archive_path,
                     const std::filesystem::path &destination) {
  static const std::set<std::string> expected{
      "encoder_fp16.onnx", "token_gather_fp16.onnx", "span_rep_fp16.onnx",
      "schema_gather_fp16.onnx", "count_pred_argmax_fp16.onnx",
      "count_lstm_fixed_fp16.onnx", "scorer_fp16.onnx",
      "classifier_fp16.onnx", "tokenizer.json"};
  int error = 0;
  zip_t *archive = zip_open(archive_path.u8string().c_str(), ZIP_RDONLY, &error);
  if (!archive) throw std::runtime_error("The downloaded model archive is invalid.");
  struct Owner { zip_t *value; ~Owner() { zip_close(value); } } owner{archive};
  const auto count = zip_get_num_entries(archive, 0);
  if (count != static_cast<zip_int64_t>(expected.size())) {
    throw std::runtime_error("The model archive has an unexpected file set.");
  }
  std::filesystem::create_directories(destination);
  std::set<std::string> found;
  std::array<char, 1 << 16> buffer{};
  for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(count); ++index) {
    zip_stat_t stat{}; zip_stat_init(&stat);
    if (zip_stat_index(archive, index, ZIP_FL_UNCHANGED, &stat) != 0 || !stat.name) {
      throw std::runtime_error("The model archive inventory is unreadable.");
    }
    const std::string name(stat.name);
    if (!expected.count(name) || name.find('/') != std::string::npos || !found.insert(name).second) {
      throw std::runtime_error("The model archive contains an unsafe path.");
    }
    zip_file_t *entry = zip_fopen_index(archive, index, ZIP_FL_UNCHANGED);
    if (!entry) throw std::runtime_error("A model archive entry is unreadable.");
    std::ofstream output(destination / name, std::ios::binary | std::ios::trunc);
    zip_uint64_t remaining = stat.size;
    while (remaining > 0) {
      const auto wanted = static_cast<zip_uint64_t>(std::min<std::uint64_t>(remaining, buffer.size()));
      const auto read = zip_fread(entry, buffer.data(), wanted);
      if (read <= 0) { zip_fclose(entry); throw std::runtime_error("A model archive entry is truncated."); }
      output.write(buffer.data(), read); remaining -= static_cast<zip_uint64_t>(read);
    }
    if (zip_fclose(entry) != 0 || !output) throw std::runtime_error("A model package file could not be written.");
  }
}

// Reports how much of a download has arrived, from the transfer's own count of
// received bytes (platform::https_download_file) - never an interpolated
// figure. Reports at most a few times a second, and only when the figure moved.
platform::DownloadProgress transfer_reporter(std::function<void(std::uint64_t)> report) {
  auto last = std::make_shared<std::uint64_t>(0);
  auto last_at = std::make_shared<std::chrono::steady_clock::time_point>();
  return [report = std::move(report), last, last_at](std::uint64_t received) {
    const auto now = std::chrono::steady_clock::now();
    if (received == *last || now - *last_at < std::chrono::milliseconds(250)) return;
    *last = received;
    *last_at = now;
    report(received);
  };
}

} // namespace

std::string model_catalog_node_id() {
  const auto value = platform::application_data_directory().string();
  return std::string(platform::contract_platform_name()) + "-" +
         std::to_string(std::hash<std::string>{}(value));
}

void ModelPackageManager::set_status(PackageSyncState state, std::string message,
                                     std::string model_id, std::string version,
                                     std::optional<int> progress_percent) {
  std::lock_guard<std::mutex> lock(mutex_);
	if (model_id.empty()) model_id = kPapinianId;
	statuses_[model_id] =
		{state, std::move(message), model_id, std::move(version), progress_percent, std::nullopt, std::nullopt};
}

void ModelPackageManager::set_transfer_status(PackageSyncState state, std::string message,
                                              const std::string &model_id, const std::string &version,
                                              std::uint64_t done_bytes, std::uint64_t total_bytes) {
  const std::uint64_t done = std::min(done_bytes, total_bytes);
  std::optional<int> percent;
  if (state == PackageSyncState::Downloading && total_bytes > 0) {
    percent = static_cast<int>(std::min<std::uint64_t>(99, (done * 100) / total_bytes));
  }
  std::lock_guard<std::mutex> lock(mutex_);
  PackageSyncStatus status{state, std::move(message), model_id, version, percent, done, total_bytes};
  statuses_[model_id] = status;
}

PackageSyncStatus ModelPackageManager::status(const std::string &model_id) const {
	const auto &package = require_manifest(model_id);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto found = statuses_.find(package.id);
		if (found != statuses_.end()) return found->second;
	}
	if (package.kind == PackageModelKind::EntityProcessing) {
		const std::lock_guard<std::mutex> verifying(verify_mutex_);
		{
			std::lock_guard<std::mutex> lock(mutex_);
			const auto found = statuses_.find(package.id);
			if (found != statuses_.end()) return found->second;
		}
		try {
			protected_api::papinian::verify_gliner2_model_package(
				protected_api::papinian::pinned_fastino_v2_model_package(package.installed_path));
			const PackageSyncStatus installed{PackageSyncState::Installed, {}, package.id, package.source_revision,
			                                  std::nullopt, std::nullopt, std::nullopt};
			// Kept as the package manager's own answer - the same one its sync
			// gives the same verified bytes - unless the sync has meanwhile said
			// something of its own.
			std::lock_guard<std::mutex> lock(mutex_);
			return statuses_.emplace(package.id, installed).first->second;
		} catch (...) {}
	} else {
		std::error_code error;
		if (std::filesystem::is_regular_file(package.installed_path, error) && !error &&
		    std::filesystem::file_size(package.installed_path, error) == package.artifact_bytes &&
		    !error && receipt_matches(package)) {
			return {PackageSyncState::Installed, {}, package.id, package.source_revision, std::nullopt, std::nullopt, std::nullopt};
		}
	}
	return {PackageSyncState::Available, {}, package.id, package.source_revision, std::nullopt, std::nullopt, std::nullopt};
}

void ModelPackageManager::sync_required_models() {
	try {
		// A missing model is fetched here only once onboarding is complete with
		// protection on (D-198). Until then its status is read again from disk,
		// where it is Available, and the member's own action fetches it.
		const auto &package = require_manifest(kPapinianId);
		std::error_code error;
		const bool present = std::filesystem::is_directory(package.installed_path, error) && !error;
		if (present == false) {
			const auto settings = platform::load_app_settings();
			const bool protection_on =
				settings.local_node_setup.protected_api_policy.mode != protected_api::Mode::Off;
			if (papinian_fetched_by_itself(settings.onboarding_completed, protection_on) == false) {
				std::lock_guard<std::mutex> lock(mutex_);
				if (running_[kPapinianId] == false) statuses_.erase(kPapinianId);
				return;
			}
		}
		install(kPapinianId);
	} catch (...) {
		// App startup must remain usable when a required privacy package cannot
		// be prepared. The per-model status retains the exact failure and every
		// protected external operation still fails closed at execution time.
	}
}

void ModelPackageManager::install(
	const std::string &model_id,
	const std::optional<std::filesystem::path> &existing_artifact) {
	const auto &package = require_manifest(model_id);
  {
    std::lock_guard<std::mutex> lock(mutex_);
		if (running_[package.id]) throw std::runtime_error("This model installation is already running.");
		running_[package.id] = true;
  }
  struct RunningReset {
		std::map<std::string, bool> &values;
		std::string id;
		std::mutex &mutex;
		~RunningReset() { std::lock_guard<std::mutex> lock(mutex); values[id] = false; }
	} reset{running_, package.id, mutex_};
  try {
	if (package.kind == PackageModelKind::EntityProcessing) {
	    const auto target = package.installed_path;
    try {
      protected_api::papinian::verify_gliner2_model_package(
          protected_api::papinian::pinned_fastino_v2_model_package(target));
      set_status(PackageSyncState::Installed, {}, kPapinianId, kPapinianVersion);
      return;
    } catch (...) {}

    const auto headers = authorized_headers();
    set_status(PackageSyncState::CheckingServer, headers.empty()
        ? "Preparing Papinian Guard's protection model…" : "Checking Legal Machines…");
    if (headers.empty()) {
      // A Debug build without a signed-in session installs the pinned package
      // from its developer copy (LocalModel/DeveloperModelSource), checked file
      // by file against the same digests the installed package is held to.
      if (const auto developer_copy = developer_model_directory(kPapinianId, kPapinianVersion)) {
        std::vector<PinnedModelFile> files;
        for (const auto &artifact : protected_api::papinian::pinned_fastino_v2_model_package(target).artifacts) {
          files.push_back({artifact.relative_path, artifact.sha256});
        }
        install_from_developer_source(*developer_copy, files, target,
            [this](DeveloperInstallStage stage, std::uint64_t done, std::uint64_t total) {
              if (stage == DeveloperInstallStage::Copying) {
                set_transfer_status(PackageSyncState::Installing, "Installing Papinian Guard's protection model…",
                                    kPapinianId, kPapinianVersion, done, total);
                return;
              }
              set_transfer_status(PackageSyncState::Verifying, "Checking Papinian Guard's protection model…",
                                  kPapinianId, kPapinianVersion, done, total);
            });
        protected_api::papinian::verify_gliner2_model_package(
            protected_api::papinian::pinned_fastino_v2_model_package(target));
        set_status(PackageSyncState::Installed, {}, kPapinianId, kPapinianVersion);
        return;
      }
      set_status(PackageSyncState::AuthorizationRequired, kSignInToDownload);
      return;
    }
    platform::HttpResponse catalog;
    try {
      catalog = platform::https_get(std::string(kApiBase) + "/local-models/catalog", headers, 20);
    } catch (const platform::HttpTransportError &error) {
      if (error.cancelled) throw;
      throw std::runtime_error(kLegalMachinesUnreachable);
    }
    // A seat with no authenticator app keeps its session, which sets one up
    // (D-215): the member sets up two-step sign-in, then tries again.
    if (node::is_two_step_setup_refusal(catalog) == true) {
      node::tell_two_step_setup_required();
      throw std::runtime_error(node::kTwoStepSetupFirst);
    }
    if (catalog.status == 401 || catalog.status == 403) {
      forget_rejected_session();
      set_status(PackageSyncState::AuthorizationRequired, kSignInAgainToDownload);
      return;
    }
    if (catalog.status < 200 || catalog.status >= 300) {
      throw std::runtime_error("Legal Machines could not offer the protection model right now. Try again in a few minutes.");
    }
    bool offered = false;
    try {
      const auto json = nlohmann::json::parse(catalog.body);
      for (const auto &model : json.at("models")) {
        if (model.value("id", "") == kPapinianId && model.value("version", "") == kPapinianVersion &&
            model.value("platform", "") == platform::contract_platform_name() &&
            model.value("archiveSha256", "") == kArchiveSha256 &&
            model.value("archiveBytes", std::uint64_t{0}) == kArchiveBytes) offered = true;
      }
    } catch (const nlohmann::json::exception &) {
      throw std::runtime_error("Legal Machines could not offer the protection model right now. Try again in a few minutes.");
    }
    if (!offered) {
      throw std::runtime_error("Legal Machines does not offer the protection model for this version of Singularity. Update Singularity, then try again.");
    }

    std::filesystem::create_directories(models_root());
    const auto suffix = random_suffix();
    const auto archive = models_root() / (std::string("download-") + suffix + ".zip");
    const auto staging = models_root() / (std::string("install-") + suffix);
    const std::string downloading = "Downloading Papinian Guard's protection model…";
    set_transfer_status(PackageSyncState::Downloading, downloading, kPapinianId, kPapinianVersion, 0, kArchiveBytes);
    int download_status = 0;
    try {
      download_status = platform::https_download_file(
          std::string(kApiBase) + "/local-models/" + kPapinianId + "/" + kPapinianVersion + "/" +
              platform::contract_platform_name(), headers, archive, kPackageDownloadTimeoutSeconds,
          transfer_reporter([this, &downloading](std::uint64_t received) {
            set_transfer_status(PackageSyncState::Downloading, downloading, kPapinianId, kPapinianVersion,
                                received, kArchiveBytes);
          }));
    } catch (const platform::HttpTransportError &error) {
      std::error_code ignored;
      std::filesystem::remove(archive, ignored);
      if (error.cancelled) throw;
      throw std::runtime_error("The download of the protection model stopped. Check the internet connection, then try again.");
    }
    if (download_status == 401 || download_status == 403) {
      std::error_code ignored;
      std::filesystem::remove(archive, ignored);
      forget_rejected_session();
      set_status(PackageSyncState::AuthorizationRequired, kSignInAgainToDownload);
      return;
    }
    if (download_status < 200 || download_status >= 300) {
      throw std::runtime_error("Legal Machines could not send the protection model right now. Try again in a few minutes.");
    }
    set_transfer_status(PackageSyncState::Verifying, "Checking the downloaded protection model…", kPapinianId,
                        kPapinianVersion, kArchiveBytes, kArchiveBytes);
    if (std::filesystem::file_size(archive) != kArchiveBytes ||
        platform::sha256_hex_digest_of_file(archive) != kArchiveSha256) {
      std::filesystem::remove(archive);
      throw std::runtime_error("The downloaded protection model was damaged on the way, so it was not installed. Try again.");
    }
    extract_package(archive, staging);
    std::filesystem::remove(archive);
    protected_api::papinian::verify_gliner2_model_package(
        protected_api::papinian::pinned_fastino_v2_model_package(staging));
    set_transfer_status(PackageSyncState::Installing, "Installing Papinian Guard's protection model…", kPapinianId,
                        kPapinianVersion, kArchiveBytes, kArchiveBytes);
    std::filesystem::create_directories(target.parent_path());
    const auto previous = models_root() / (std::string("previous-") + suffix);
    std::error_code ignored;
    if (std::filesystem::exists(target)) std::filesystem::rename(target, previous);
    try {
      std::filesystem::rename(staging, target);
      std::filesystem::remove_all(previous, ignored);
    } catch (...) {
      if (std::filesystem::exists(previous) && !std::filesystem::exists(target)) {
        std::filesystem::rename(previous, target, ignored);
      }
      throw;
    }
    set_status(PackageSyncState::Installed, {}, kPapinianId, kPapinianVersion);
		return;
	}

	try {
		verify_file(package.installed_path, package);
		if (!receipt_matches(package)) write_receipt(package);
		set_status(PackageSyncState::Installed, {}, package.id, package.source_revision);
		return;
	} catch (...) {}

	set_status(PackageSyncState::CheckingServer, "Preparing verified model source…",
	           package.id, package.source_revision);
	std::filesystem::create_directories(package_cache_root());
	const auto cache = package_cache_root() /
		(package.artifact_sha256 + "-" + package.artifact_filename);
	bool cache_ready = false;
	if (std::filesystem::exists(cache)) {
		try {
			verify_file(cache, package);
			cache_ready = true;
		} catch (...) {
			std::filesystem::remove(cache);
		}
	}

	if (!cache_ready) {
		const auto suffix = random_suffix();
		const auto staging_cache = cache.string() + ".download-" + suffix;
		if (existing_artifact) {
			set_status(PackageSyncState::Verifying, "Verifying downloaded model…",
			           package.id, package.source_revision);
			verify_file(*existing_artifact, package);
			std::filesystem::copy_file(*existing_artifact, staging_cache,
			                           std::filesystem::copy_options::overwrite_existing);
		} else {
			// Beta does not download large generative models itself: a person
			// who has the model chooses its file, which is verified exactly as a
			// download would be (the branch above).
			if (package.kind == PackageModelKind::Generative) throw std::runtime_error(kChooseExistingFile);
			set_transfer_status(PackageSyncState::Downloading, "Downloading Qwen3 30B A3B…",
			                    package.id, package.source_revision, 0, package.artifact_bytes);
			const int download_status = platform::https_download_file(package.source_url, {}, staging_cache,
				kPackageDownloadTimeoutSeconds,
				transfer_reporter([this, &package](std::uint64_t received) {
					set_transfer_status(PackageSyncState::Downloading, "Downloading Qwen3 30B A3B…",
					                    package.id, package.source_revision, received, package.artifact_bytes);
				}));
			if (download_status < 200 || download_status >= 300) {
				std::filesystem::remove(staging_cache);
				throw std::runtime_error("The model download failed.");
			}
		}
		set_status(PackageSyncState::Verifying, "Verifying model integrity…",
		           package.id, package.source_revision);
		try {
			verify_file(staging_cache, package);
			std::filesystem::rename(staging_cache, cache);
		} catch (...) {
			std::filesystem::remove(staging_cache);
			throw;
		}
	}

	set_status(PackageSyncState::Installing, "Installing Qwen3 30B A3B…",
	           package.id, package.source_revision);
	std::filesystem::create_directories(package.installed_path.parent_path());
	const auto suffix = random_suffix();
	const auto staging_target = package.installed_path.string() + ".install-" + suffix;
	std::error_code link_error;
	std::filesystem::create_hard_link(cache, staging_target, link_error);
	if (link_error) {
		std::filesystem::copy_file(cache, staging_target,
		                           std::filesystem::copy_options::overwrite_existing);
		verify_file(staging_target, package);
	}
	const auto previous = package.installed_path.string() + ".previous-" + suffix;
	std::error_code ignored;
	if (std::filesystem::exists(package.installed_path)) {
		std::filesystem::rename(package.installed_path, previous);
	}
	try {
		std::filesystem::rename(staging_target, package.installed_path);
		write_receipt(package);
		std::filesystem::remove(previous, ignored);
	} catch (...) {
		std::filesystem::remove(staging_target, ignored);
		if (std::filesystem::exists(previous) && !std::filesystem::exists(package.installed_path)) {
			std::filesystem::rename(previous, package.installed_path, ignored);
		}
		throw;
	}
	set_status(PackageSyncState::Installed, {}, package.id, package.source_revision);
	} catch (const std::exception &error) {
		set_status(PackageSyncState::Error, error.what(), package.id,
		           package.source_revision);
		throw;
  }
}

void ModelPackageManager::remove(const std::string &model_id) {
	const auto &package = require_manifest(model_id);
	if (package.required) throw std::runtime_error("This required local model cannot be removed.");
	std::error_code error;
	if (!std::filesystem::is_regular_file(package.installed_path, error) || error) {
		throw std::runtime_error("This local model is not installed.");
	}
	if (!std::filesystem::remove(package.installed_path, error) || error) {
		throw std::runtime_error("The installed model could not be removed.");
	}
	std::filesystem::remove(receipt_path(package.id), error);
	set_status(PackageSyncState::Available, {}, package.id, package.source_revision);
}

std::optional<ModelPackageManifest> ModelPackageManager::manifest(
	const std::string &model_id) const {
	try {
		return require_manifest(model_id);
	} catch (...) {
		return std::nullopt;
	}
}

std::vector<ModelPackageManifest> ModelPackageManager::catalog() const {
	return catalog_manifests();
}

bool ModelPackageManager::installed_and_verified(const std::string &model_id) const {
	const auto &package = require_manifest(model_id);
	if (package.kind == PackageModelKind::EntityProcessing) {
		try {
			protected_api::papinian::verify_gliner2_model_package(
				protected_api::papinian::pinned_fastino_v2_model_package(package.installed_path));
			return true;
		} catch (...) {
			return false;
		}
	}
	try {
		verify_file(package.installed_path, package);
		return receipt_matches(package);
	} catch (...) {
		return false;
	}
}

void ModelPackageManager::authorize_and_sync(const std::string &password, const std::string &challenge_id) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto running = running_.find(kPapinianId);
    if (running != running_.end() && running->second == true) return;
  }
  const auto settings = platform::load_app_settings();
  if (!settings.account || password.empty()) throw std::runtime_error("An Singularity account and password are required.");
  set_status(PackageSyncState::CheckingServer, "Signing this computer in to Legal Machines…", kPapinianId,
             kPapinianVersion);
  const node::AccountPoster post = [](const std::string &url, const std::string &body) {
    return platform::https_post(url, {{"Content-Type", "application/json"}}, body, 30);
  };
  std::string token;
  try {
    token = node::exchange_node_session(post, settings.account->organisation_id, settings.account->seat_id, password,
                                        model_catalog_node_id(), challenge_id);
  } catch (const node::AccountFailure &failure) {
    // A refused password, a code still to be checked or an email still to be
    // confirmed is put right by signing in again; anything else is retried.
    const node::AccountFailureKind kind = failure.getKind();
    const PackageSyncState state = kind == node::AccountFailureKind::WrongCredentials
            || kind == node::AccountFailureKind::CodeRequired
            || kind == node::AccountFailureKind::OrganisationNotActive
            || kind == node::AccountFailureKind::AwaitingApproval
        ? PackageSyncState::AuthorizationRequired : PackageSyncState::Error;
    set_status(state, failure.what(), kPapinianId, kPapinianVersion);
    throw std::runtime_error(failure.what());
  }
  // The token goes straight to the native credential store and is dropped
  // here: nothing a person, a trace or a setting can read ever holds it.
  try {
    platform::set_provider_credential(kSessionCredential, token);
  } catch (const std::exception &) {
    token.assign(token.size(), '\0');
    const std::string message = "Singularity could not keep this computer's Legal Machines sign-in. Try again.";
    set_status(PackageSyncState::Error, message, kPapinianId, kPapinianVersion);
    throw std::runtime_error(message);
  }
  token.assign(token.size(), '\0');
  // With the session kept, a missing model downloads now only once onboarding
  // is complete with protection on (D-198); in onboarding the member's choice
  // starts it.
  sync_required_models();
}

std::vector<platform::HttpHeader> ModelPackageManager::session_headers() const {
  try {
    return authorized_headers();
  } catch (...) {
    return {};
  }
}

bool ModelPackageManager::has_session() const {
  try {
    const auto token = platform::get_provider_credential(kSessionCredential);
    return token.has_value() && token->empty() == false;
  } catch (...) {
    return false;
  }
}

void ModelPackageManager::forget_session() {
  // The session ends at Legal Machines first: only it, with the seat it
  // belongs to, is sent, and the answer is never waited on for long nor read.
  try {
    if (const std::optional<SessionEndRequest> end = session_end_request(authorized_headers()))
      (void)platform::https_post(end->url, end->headers, end->body, 5);
  } catch (...) {
  }
  platform::delete_provider_credential(kSessionCredential);
  std::lock_guard<std::mutex> lock(mutex_);
  if (running_[kPapinianId] == true) return;
  const auto found = statuses_.find(kPapinianId);
  if (found != statuses_.end() && found->second.state == PackageSyncState::Installed) return;
  statuses_[kPapinianId] = {PackageSyncState::AuthorizationRequired, kSignInToDownload, kPapinianId,
                            kPapinianVersion, std::nullopt, std::nullopt, std::nullopt};
}

bool papinian_developer_copy_available() {
  return developer_model_directory(kPapinianId, kPapinianVersion).has_value();
}

std::string papinian_package_version() { return kPapinianVersion; }
std::string papinian_model_id() { return kPapinianId; }
std::uint64_t papinian_archive_bytes() { return kArchiveBytes; }
std::string qwen_context_model_id() { return kQwenContextId; }

ModelPackageManager &production_model_package_manager() {
  static ModelPackageManager manager;
  return manager;
}

} // namespace singularity::local_models
