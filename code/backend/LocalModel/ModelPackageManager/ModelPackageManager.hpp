#pragma once

#include "Node/LegalMachinesConnection/LegalMachinesConnection.hpp"
#include "Platform/HttpClient/HttpClient.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace singularity::local_models {

// The request that ends this computer's session at Legal Machines when the
// person signs out: the session's own headers, nothing else. None for a
// computer that holds no session. Pure, so what leaves is checked by a test.
struct SessionEndRequest {
  std::string url;
  std::vector<platform::HttpHeader> headers;
  std::string body;
};
inline std::optional<SessionEndRequest> session_end_request(const std::vector<platform::HttpHeader> &session_headers) {
  if (session_headers.empty()) return std::nullopt;
  SessionEndRequest request;
  request.url = std::string(node::kLegalMachinesApiBase) + "/auth/sign-out";
  request.headers = session_headers;
  request.headers.push_back({"Content-Type", "application/json"});
  request.body = "{}";
  return request;
}

enum class PackageSyncState {
	Available,
  CheckingServer,
  AuthorizationRequired,
  Downloading,
  Verifying,
  Installing,
  Installed,
  Error,
};

struct PackageSyncStatus {
  PackageSyncState state = PackageSyncState::CheckingServer;
  std::string message;
  std::string model_id;
  std::string version;
  // Populated only during PackageSyncState::Downloading, from real bytes
  // already received against the pinned archive size - never a
  // fabricated figure. 0-99; Verifying/Installing leave this unset again
  // since no further per-byte signal exists past the download itself.
  std::optional<int> progress_percent;
  // The bytes of the package that have arrived, and how many it has in all:
  // set while it downloads, and - the download then being whole - while it is
  // checked and installed. Unset whenever no transfer is under way.
  std::optional<std::uint64_t> done_bytes;
  std::optional<std::uint64_t> total_bytes;
};

// The one way this computer can get the protection model while it has none,
// from the package's state and what this computer holds: "install_here" from a
// Debug build's developer copy when no Legal Machines session exists,
// "retry_download" with a session, and otherwise "sign_in" - which the
// interface says in words, never as a button, until sign-in exists. A session
// Legal Machines turned away (AuthorizationRequired while one is still held)
// is never offered again: trying it again would only be turned away again.
inline const char *papinian_recovery_for(PackageSyncState state, bool has_session,
                                         bool developer_copy_available) {
  if (has_session == false) return developer_copy_available ? "install_here" : "sign_in";
  if (state == PackageSyncState::AuthorizationRequired) return "sign_in";
  return "retry_download";
}

// Whether Singularity fetches a missing protection model by itself, at
// sign-in or at launch: only once onboarding is complete and protection is
// on. In onboarding the member's own choice and button fetch it
// (localModels.sync); with protection off nothing downloads until someone
// asks. An installed model is still checked, and repaired.
inline bool papinian_fetched_by_itself(bool onboarding_completed, bool protection_on) {
  return onboarding_completed == true && protection_on == true;
}

enum class PackageModelKind { EntityProcessing, Generative };

// Immutable production catalogue entry. The artifact fields describe the
// exact bytes Node is allowed to install; the source revision is pinned so a
// mutable remote branch can never silently change what an Install action
// means. `runtime_model_id` is the stable id Singularity derives from those bytes
// and is also the ModelTarget id AI Setup persists.
struct ModelPackageManifest {
	std::string id;
	std::string runtime_model_id;
	std::string display_name;
	PackageModelKind kind = PackageModelKind::Generative;
	std::vector<std::string> capabilities;
	std::string format;
	std::string source_repository;
	std::string source_revision;
	std::string source_url;
	std::string artifact_filename;
	std::uint64_t artifact_bytes = 0;
	std::string artifact_sha256;
	std::string license;
	std::string license_url;
	std::filesystem::path installed_path;
	bool required = false;
};

class ModelPackageManager {
public:
  void sync_required_models();
  // Exchanges the account password for this computer's download session and
  // then downloads what is required. The password is sent to Legal Machines
  // once, over HTTPS, and never kept. Does nothing while a required package is
  // already being installed: a download under way already has its session.
  // `challenge_id` names the sign-in whose code was just checked; empty when
  // there was none (the service then asks for one when the seat needs it).
  void authorize_and_sync(const std::string &password, const std::string &challenge_id = "");
  // Whether this computer holds a download session. Never the session itself.
  bool has_session() const;
  // The headers a request of this computer's session carries to Legal
  // Machines (its bearer and whose it is), as the downloads send them; empty
  // without a session. For the account's own requests, such as inviting a
  // colleague; never shown or kept elsewhere.
  std::vector<platform::HttpHeader> session_headers() const;
  // Signing out: asks Legal Machines to end this computer's session, then
  // forgets it here whatever the answer, so a sign-out never fails for the
  // network. An installed package stays installed; one that is not says it
  // needs a sign-in again.
  void forget_session();
	// Installs any catalogue model. Passing an existing artifact is the
	// production "Use downloaded file" path: it receives the exact same size,
	// SHA-256, cache, atomic install and receipt checks as a network download.
	void install(const std::string &model_id,
	             const std::optional<std::filesystem::path> &existing_artifact = std::nullopt);
	void remove(const std::string &model_id);
	PackageSyncStatus status(const std::string &model_id) const;
	std::optional<ModelPackageManifest> manifest(const std::string &model_id) const;
	std::vector<ModelPackageManifest> catalog() const;
	bool installed_and_verified(const std::string &model_id) const;

private:
  void set_status(PackageSyncState state, std::string message = {},
                  std::string model_id = {}, std::string version = {},
                  std::optional<int> progress_percent = std::nullopt);
  // The same, carrying how much of the package has arrived.
  void set_transfer_status(PackageSyncState state, std::string message, const std::string &model_id,
                           const std::string &version, std::uint64_t done_bytes, std::uint64_t total_bytes);
  mutable std::mutex mutex_;
	std::map<std::string, bool> running_;
	// Mutable so a status read that verified an installed package keeps that
	// answer: the next read (startup polls every 300 ms) does not hash the
	// whole package again.
	mutable std::map<std::string, PackageSyncStatus> statuses_;
	// One status read verifies an installed package at a time; the others then
	// find its answer instead of hashing the same bytes alongside it.
	mutable std::mutex verify_mutex_;
};

ModelPackageManager &production_model_package_manager();
std::string model_catalog_node_id();
std::string papinian_package_version();
// Whether this build can install the protection model from a developer copy on
// this computer, without a Legal Machines session (Debug builds only - see
// LocalModel/DeveloperModelSource).
bool papinian_developer_copy_available();
// The stable catalogue id Model Library cards match this package manager's
// live status against - see Bridge/LocalModelBridge/LocalModelBridge.cpp's
// card-state merge.
std::string papinian_model_id();
// The pinned archive size, in bytes - what a not-yet-installed card shows
// as the download size, and the denominator for real download progress.
std::uint64_t papinian_archive_bytes();

std::string qwen_context_model_id();

} // namespace singularity::local_models
