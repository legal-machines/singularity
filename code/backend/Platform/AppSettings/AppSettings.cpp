#include "Platform/Trace/Trace.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/AppSettings/AppSettings.hpp"

#include <cctype>

#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"

#include <nlohmann/json.hpp>

#include <mutex>
#include <algorithm>
#include <atomic>
#include <memory>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace singularity::platform {

const char *to_string(FilesAccessDecision decision) {
	switch (decision) {
		case FilesAccessDecision::NotAsked:
			return "not-asked";
		case FilesAccessDecision::Declined:
			return "declined";
		case FilesAccessDecision::Granted:
			return "granted";
	}
	return "not-asked";
}

FilesAccessDecision files_access_decision_from_string(const std::string &value) {
	if (value == "declined") return FilesAccessDecision::Declined;
	if (value == "granted") return FilesAccessDecision::Granted;
	return FilesAccessDecision::NotAsked;
}

const char *to_string(BroadProjectDiscoveryAuthorization authorization) {
	switch (authorization) {
		case BroadProjectDiscoveryAuthorization::Authorized:
			return "authorized";
		case BroadProjectDiscoveryAuthorization::NotAuthorized:
			return "not-authorized";
	}
	return "not-authorized";
}

BroadProjectDiscoveryAuthorization broad_project_discovery_authorization_from_string(const std::string &value) {
	if (value == "authorized") return BroadProjectDiscoveryAuthorization::Authorized;
	return BroadProjectDiscoveryAuthorization::NotAuthorized;
}

const char *to_string(NodeSetupStep step) {
  switch (step) {
    case NodeSetupStep::Workspace:
      return "workspace";
    case NodeSetupStep::Models:
      return "models";
    case NodeSetupStep::ProtectedApi:
      return "protected-api";
    case NodeSetupStep::Ready:
      return "ready";
  }
  return "workspace";
}

NodeSetupStep node_setup_step_from_string(const std::string &value) {
  if (value == "models") return NodeSetupStep::Models;
  // "privacy" and "protectedApi" are pre-rename values written by older
  // settings.json files (see LocalNodeSetup::version); accepted here so an
  // interrupted setup resumes at the right screen instead of restarting.
  if (value == "protected-api" || value == "protectedApi" || value == "privacy") {
    return NodeSetupStep::ProtectedApi;
  }
  if (value == "ready") return NodeSetupStep::Ready;
  // Unrecognized values, including the removed "language" step, fall back
  // to the first remaining Phase B step rather than failing to load.
  return NodeSetupStep::Workspace;
}

namespace {

using Json = nlohmann::json;

// The settings file format this build writes. A file a newer build wrote keeps
// its higher number: an older build never lowers it.
constexpr int kSettingsSchemaVersion = 1;

// Settings are read field by field: a value of the wrong type reads as absent
// instead of failing the whole file, so one hand edit or a field a newer build
// changed never resets every other setting to defaults.
bool bool_field(const Json &object, const char *key, bool fallback) {
  if (!object.is_object()) return fallback;
  const auto found = object.find(key);
  return found != object.end() && found->is_boolean() ? found->get<bool>() : fallback;
}

std::string string_field(const Json &object, const char *key, const std::string &fallback) {
  if (!object.is_object()) return fallback;
  const auto found = object.find(key);
  return found != object.end() && found->is_string() ? found->get<std::string>() : fallback;
}

int int_field(const Json &object, const char *key, int fallback) {
  if (!object.is_object()) return fallback;
  const auto found = object.find(key);
  return found != object.end() && found->is_number_integer() ? found->get<int>() : fallback;
}

// Keys earlier builds wrote and this build migrated away from; they are not
// carried forward into a new save.
bool retired_local_setup_key(const std::string &key) {
  return key == "workspacePath" || key == "protectedApiDefault";
}

std::filesystem::path previous_path(const std::filesystem::path &path) {
  return path.string() + ".previous";
}

std::filesystem::path unique_temporary_path(const std::filesystem::path &path) {
  std::random_device seed;
  std::mt19937_64 generator(seed());
  return path.string() + ".tmp-" + std::to_string(generator());
}

// Settings are saved the way every file Singularity writes is (Platform/AtomicFile):
// the bytes on the disk before the rename that puts them in place, a full disk
// refused as DiskFull in plain words, and no path in any message.
void atomic_replace(const std::filesystem::path &source, const std::filesystem::path &destination) {
  commit_staged_file(source, destination, StagedCommitMode::ReplaceExisting, false);
}

Json read_json_file(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("Singularity could not read its settings on this computer.");
  }
  return Json::parse(file);
}

void write_json_file(const std::filesystem::path &path, const std::string &text) {
  write_file_durably(path, text);
}

Json local_workspace_to_json(const LocalWorkspace &workspace) {
  return Json{
      {"id", workspace.id},
      {"displayName", workspace.display_name},
      {"path", workspace.path},
      {"createdAt", workspace.created_at},
      {"lastOpenedAt", workspace.last_opened_at ? Json(*workspace.last_opened_at) : Json(nullptr)},
  };
}

LocalWorkspace local_workspace_from_json(const Json &value) {
  LocalWorkspace workspace;
  workspace.id = string_field(value, "id", "");
  workspace.display_name = string_field(value, "displayName", "");
  workspace.path = string_field(value, "path", "");
  workspace.created_at = string_field(value, "createdAt", "");
  if (value.contains("lastOpenedAt") && value.at("lastOpenedAt").is_string()) {
    workspace.last_opened_at = value.at("lastOpenedAt").get<std::string>();
  }
  return workspace;
}

Json model_descriptor_to_json(const providers::ModelDescriptor &model) {
  return Json{
      {"id", model.id},
      {"displayName", model.display_name ? Json(*model.display_name) : Json(nullptr)},
      {"textCapable", model.text_capable},
      {"excludedReason", model.excluded_reason ? Json(*model.excluded_reason) : Json(nullptr)},
      {"contextWindowTokens",
          model.context_window_tokens ? Json(*model.context_window_tokens) : Json(nullptr)},
      {"maxOutputTokens", model.max_output_tokens ? Json(*model.max_output_tokens) : Json(nullptr)},
      {"inputPricePerToken", model.input_price_per_token ? Json(*model.input_price_per_token) : Json(nullptr)},
      {"outputPricePerToken", model.output_price_per_token ? Json(*model.output_price_per_token) : Json(nullptr)},
      {"discoveredAt", model.discovered_at},
      {"supportedParameters", model.supported_parameters},
      {"supportsStructuredOutput", model.supports_structured_output},
      {"supportsNativeTools", model.supports_native_tools},
      {"supportsToolChoice", model.supports_tool_choice},
  };
}

// Accepts either the current object shape or a legacy bare model-id string
// (settings.json written before this field carried classification), so an
// existing connected provider is not dropped just because ModelDescriptor
// grew richer than a plain id.
providers::ModelDescriptor model_descriptor_from_json(const Json &value) {
  providers::ModelDescriptor model;
  if (value.is_string()) {
    model.id = value.get<std::string>();
    model.excluded_reason = providers::classify_model_exclusion(model.id);
    model.text_capable = !model.excluded_reason.has_value();
    return model;
  }
  model.id = string_field(value, "id", "");
  if (value.contains("displayName") && value.at("displayName").is_string()) {
    model.display_name = value.at("displayName").get<std::string>();
  }
  model.text_capable = bool_field(value, "textCapable", true);
  if (value.contains("excludedReason") && value.at("excludedReason").is_string()) {
    model.excluded_reason = value.at("excludedReason").get<std::string>();
  }
  if (value.contains("contextWindowTokens") && value.at("contextWindowTokens").is_number_unsigned()) {
    model.context_window_tokens = value.at("contextWindowTokens").get<std::uint64_t>();
  }
  // The provider's reported output limit and prices, and when they were read;
  // absent in settings written before they were kept.
  if (value.contains("maxOutputTokens") && value.at("maxOutputTokens").is_number_unsigned()) {
    model.max_output_tokens = value.at("maxOutputTokens").get<std::uint64_t>();
  }
  if (value.contains("inputPricePerToken") && value.at("inputPricePerToken").is_number()) {
    model.input_price_per_token = value.at("inputPricePerToken").get<double>();
  }
  if (value.contains("outputPricePerToken") && value.at("outputPricePerToken").is_number()) {
    model.output_price_per_token = value.at("outputPricePerToken").get<double>();
  }
  model.discovered_at = string_field(value, "discoveredAt", "");
  if (value.contains("supportedParameters") && value.at("supportedParameters").is_array()) {
    for (const auto &parameter : value.at("supportedParameters"))
      if (parameter.is_string()) model.supported_parameters.push_back(parameter.get<std::string>());
  }
  if (value.contains("supportsStructuredOutput") && value.at("supportsStructuredOutput").is_boolean()) {
    model.supports_structured_output = value.at("supportsStructuredOutput").get<bool>();
  }
  // Absent (a settings.json persisted before Phase 24.1D introduced these
  // fields) means false, exactly like every other capability field's
  // legacy-migration default above - never re-derived from
  // supportedParameters here, since this is a pure read-back of whatever
  // discovery last persisted, not a second derivation site duplicating
  // ProviderRegistry.cpp's own derive_supports_native_tools()/
  // derive_supports_tool_choice(). A stale legacy entry updates the next
  // time discovery actually runs (see refresh_providers()).
  if (value.contains("supportsNativeTools") && value.at("supportsNativeTools").is_boolean()) {
    model.supports_native_tools = value.at("supportsNativeTools").get<bool>();
  }
  if (value.contains("supportsToolChoice") && value.at("supportsToolChoice").is_boolean()) {
    model.supports_tool_choice = value.at("supportsToolChoice").get<bool>();
  }
  return model;
}

Json connected_provider_to_json(const ConnectedProvider &provider) {
  Json models = Json::array();
  for (const auto &model : provider.models) {
    models.push_back(model_descriptor_to_json(model));
  }
  return Json{
      {"id", provider.id},
      {"baseUrl", provider.base_url ? Json(*provider.base_url) : Json(nullptr)},
      {"models", models},
      {"displayName", provider.display_name ? Json(*provider.display_name) : Json(nullptr)},
  };
}

ConnectedProvider connected_provider_from_json(const Json &value) {
  ConnectedProvider provider;
  provider.id = string_field(value, "id", "");
  if (value.contains("baseUrl") && value.at("baseUrl").is_string()) {
    provider.base_url = value.at("baseUrl").get<std::string>();
  }
  if (value.contains("models") && value.at("models").is_array()) {
    for (const auto &model : value.at("models")) {
      if (model.is_string() || model.is_object()) {
        provider.models.push_back(model_descriptor_from_json(model));
      }
    }
  }
  if (value.contains("displayName") && value.at("displayName").is_string()) {
    provider.display_name = value.at("displayName").get<std::string>();
  }
  return provider;
}

Json role_model_assignment_to_json(const RoleModelAssignment &assignment) {
  return Json{
      {"role", assignment.role},
      {"providerId", assignment.provider_id},
      {"modelId", assignment.model_id},
  };
}

std::optional<RoleModelAssignment> role_model_assignment_from_json(const Json &value) {
  if (!value.is_object()) return std::nullopt;
  RoleModelAssignment assignment;
  assignment.role = string_field(value, "role", "");
  assignment.provider_id = string_field(value, "providerId", "");
  assignment.model_id = string_field(value, "modelId", "");
  if (assignment.role.empty() || assignment.provider_id.empty() || assignment.model_id.empty()) {
    return std::nullopt;
  }
  return assignment;
}

Json protected_api_policy_to_json(const protected_api::Policy &policy) {
  Json custom_categories = Json::array();
  for (const auto category : policy.custom_categories) {
    custom_categories.push_back(protected_api::to_string(category));
  }
  return Json{
      {"mode", protected_api::to_string(policy.mode)},
      {"customCategories", std::move(custom_categories)},
      {"customTerms", policy.custom_terms},
  };
}

protected_api::Policy protected_api_policy_from_json(const Json &value) {
  protected_api::Policy policy;
  policy.mode = protected_api::mode_from_string(string_field(value, "mode", "standard"));
  bool unknown_category = false;
  if (value.contains("customCategories") && value.at("customCategories").is_array()) {
    for (const auto &category : value.at("customCategories")) {
      if (!category.is_string()) continue;
      const std::string name = category.get<std::string>();
      const auto &known = protected_api::all_categories();
      if (std::none_of(known.begin(), known.end(),
              [&](protected_api::Category candidate) { return name == protected_api::to_string(candidate); })) {
        unknown_category = true;
        continue;
      }
      policy.custom_categories.push_back(protected_api::category_from_string(name));
    }
  }
  // A custom selection naming a category this build does not know is never
  // narrowed to the categories it does: protection widens to Standard, which
  // covers every category this build can protect.
  if (unknown_category && policy.mode == protected_api::Mode::Custom) {
    policy.mode = protected_api::Mode::Standard;
    policy.custom_categories.clear();
  }
  if (value.contains("customTerms") && value.at("customTerms").is_array()) {
    for (const auto &term : value.at("customTerms")) {
      if (term.is_string()) {
        policy.custom_terms.push_back(term.get<std::string>());
      }
    }
  }
  return policy;
}

// Reads a pre-v2 settings.json's single `workspacePath` string, if any, and
// turns it into the one-element `workspaces` list a fresh v2 install would
// have. Existing dev installs keep whatever workspace they had configured
// instead of losing it on the next launch after this upgrade.
void migrate_v1_workspace(const Json &local, LocalNodeSetup &setup) {
  if (!local.contains("workspacePath") || !local.at("workspacePath").is_string()) {
    return;
  }
  const auto path = local.at("workspacePath").get<std::string>();
  if (path.empty()) return;

  LocalWorkspace workspace;
  workspace.id = workspaces::generate_workspace_id();
  workspace.display_name = workspaces::derive_display_name(std::filesystem::path(path));
  workspace.path = path;
  workspace.created_at = workspaces::current_timestamp();
  setup.workspaces.push_back(workspace);
  setup.active_workspace_id = workspace.id;
}

// Reads a pre-v2 settings.json's boolean `protectedApiDefault`, if any,
// mapping it onto the new Standard/Off policy mode.
void migrate_v1_protected_api(const Json &local, LocalNodeSetup &setup) {
  if (!local.contains("protectedApiDefault") || !local.at("protectedApiDefault").is_boolean()) {
    return;
  }
  setup.protected_api_policy.mode =
      local.at("protectedApiDefault").get<bool>() ? protected_api::Mode::Standard : protected_api::Mode::Off;
}

// role_assignments (not default_model) is the canonical Node default AI
// Setup for every Beta 1 role, including Reasoning (Phase 06,
// AI_SETUP_CONTRACT.md) - there must never be two independently writable
// persisted sources of truth for Reasoning. This keeps the two in sync on
// every load: a pre-Phase-06 settings.json only ever set default_model, so
// its value becomes role_assignments' own Reasoning entry exactly once;
// once role_assignments carries a real Reasoning entry, that entry is
// authoritative and default_model is recomputed from it, never the reverse.
// Deliberately does not call Routing::parse_model_target() - Platform must
// not depend on the Routing domain built on top of it.
void synchronize_reasoning_role_assignment(LocalNodeSetup &setup) {
  const auto reasoning = std::find_if(setup.role_assignments.begin(), setup.role_assignments.end(),
      [](const RoleModelAssignment &assignment) { return assignment.role == "reasoning"; });
  if (reasoning != setup.role_assignments.end()) {
    setup.default_model = reasoning->provider_id + ":" + reasoning->model_id;
    return;
  }
  if (!setup.default_model || setup.default_model->empty()) return;
  const auto separator = setup.default_model->find(':');
  if (separator == std::string::npos || separator == 0 || separator == setup.default_model->size() - 1) return;
  setup.role_assignments.push_back(RoleModelAssignment{
      "reasoning", setup.default_model->substr(0, separator), setup.default_model->substr(separator + 1)});
}

std::filesystem::path require_env_path(const char *name) {
  const auto *value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    throw std::runtime_error(
        std::string("Required environment variable is not set: ") + name);
  }
  return std::filesystem::path(value);
}

} // namespace

std::filesystem::path app_settings_directory() {
#if defined(SINGULARITY_ISOLATED_TEST_SETTINGS)
  return require_env_path("SINGULARITY_TEST_SETTINGS_DIRECTORY");
#elif defined(__APPLE__)
  // The application's one folder in Application Support, its data's too
  // (Platform/AppPaths): macOS names folders whatever their letters' case.
  return require_env_path("HOME") / "Library" / "Application Support" /
         NATIVE_APP_DATA_NAME;
#elif defined(_WIN32)
  return require_env_path("APPDATA") / NATIVE_APP_SLUG;
#elif defined(__linux__)
  if (const auto *xdg_config = std::getenv("XDG_CONFIG_HOME");
      xdg_config != nullptr && *xdg_config != '\0') {
    return std::filesystem::path(xdg_config) / NATIVE_APP_SLUG;
  }
  return require_env_path("HOME") / ".config" / NATIVE_APP_SLUG;
#else
#error "Only macOS, Windows and Linux are supported"
#endif
}

std::filesystem::path app_settings_file_path() {
  return app_settings_directory() / "settings.json";
}

std::filesystem::path computer_records_directory() {
#if defined(SINGULARITY_ISOLATED_TEST_SETTINGS)
  return app_settings_directory();
#elif defined(_WIN32)
  // The folder of this computer's application data (Platform/AppPaths
  // application_data_directory()), so all it owns lives in one place.
  return require_env_path("LOCALAPPDATA") / NATIVE_APP_DATA_NAME;
#else
  return app_settings_directory();
#endif
}

namespace {

// What a settings file looked like when it was last read: present or not, and
// if present its size and modification time.
struct SettingsFileStamp {
  bool present = false;
  std::uintmax_t size = 0;
  std::filesystem::file_time_type modified{};
  bool operator==(const SettingsFileStamp &other) const {
    return present == other.present && size == other.size && modified == other.modified;
  }
};

SettingsFileStamp settings_stamp_of(const std::filesystem::path &path) {
  SettingsFileStamp stamp;
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) return stamp;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) return stamp;
  const auto modified = std::filesystem::last_write_time(path, error);
  if (error) return stamp;
  stamp.present = true;
  stamp.size = size;
  stamp.modified = modified;
  return stamp;
}

struct KeptSettings {
  std::mutex mutex;
  bool filled = false;
  std::filesystem::path path;
  SettingsFileStamp current;
  SettingsFileStamp previous;
  std::shared_ptr<const AppSettings> settings;
};

KeptSettings &kept_settings() {
  static KeptSettings *kept = new KeptSettings();
  return *kept;
}

// Saves this process has begun and finished, counted twice each: a read scope
// that took its settings before the count it holds changed reads them again.
std::atomic<std::uint64_t> &settings_saves() {
  static std::atomic<std::uint64_t> *saves = new std::atomic<std::uint64_t>(0);
  return *saves;
}

// The settings this thread's read scope took (AppSettingsReadScope).
struct ScopedSettings {
  int depth = 0;
  std::filesystem::path path;
  std::uint64_t saves = 0;
  std::shared_ptr<const AppSettings> settings;
  std::size_t reads = 0;
  std::chrono::steady_clock::time_point began;
};

ScopedSettings &scoped_settings() {
  thread_local ScopedSettings scope;
  return scope;
}

AppSettings load_app_settings_from_disk();

} // namespace

// Settings are read many times while one request runs, and the file - which
// carries every connected provider's model list - takes tens of milliseconds
// to parse. The parsed result is kept and returned for as long as the file
// (and its recovery copy) is unchanged on disk; any write, by this process or
// another, changes the file's stamp and the next read parses it again. Inside
// a read scope the thread does not look at the file again at all until this
// process saves settings (AppSettingsReadScope).
std::shared_ptr<const AppSettings> load_app_settings_shared() {
  const auto path = app_settings_file_path();
  ScopedSettings &scope = scoped_settings();
  const std::uint64_t saves = settings_saves().load();
  if (scope.depth > 0 && scope.settings && scope.saves == saves && scope.path == path) {
    ++scope.reads;
    return scope.settings;
  }
  trace::Span span("persist", "app_settings.load");
  const SettingsFileStamp current = settings_stamp_of(path);
  const SettingsFileStamp previous = settings_stamp_of(previous_path(path));
  KeptSettings &kept = kept_settings();
  std::shared_ptr<const AppSettings> settings;
  {
    const std::lock_guard<std::mutex> lock(kept.mutex);
    if (kept.filled && kept.path == path && kept.current == current && kept.previous == previous) {
      span.add("path", "kept");
      settings = kept.settings;
    }
  }
  if (!settings) {
    settings = std::make_shared<const AppSettings>(load_app_settings_from_disk());
    span.add("path", "parsed");
    // Kept only if the file did not change while it was being read.
    if (settings_stamp_of(path) == current && settings_stamp_of(previous_path(path)) == previous) {
      const std::lock_guard<std::mutex> lock(kept.mutex);
      kept.filled = true;
      kept.path = path;
      kept.current = current;
      kept.previous = previous;
      kept.settings = settings;
    }
  }
  if (scope.depth > 0) {
    scope.path = path;
    scope.saves = saves;
    scope.settings = settings;
  }
  return settings;
}

AppSettings load_app_settings() {
  return *load_app_settings_shared();
}

AppSettingsReadScope::AppSettingsReadScope() {
  ScopedSettings &scope = scoped_settings();
  if (scope.depth++ > 0) return;
  scope.settings.reset();
  scope.reads = 0;
  scope.began = std::chrono::steady_clock::now();
}

AppSettingsReadScope::~AppSettingsReadScope() {
  ScopedSettings &scope = scoped_settings();
  if (--scope.depth > 0) return;
  trace::record("persist", "app_settings.scope", {{"reads", scope.reads}},
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - scope.began).count());
  scope.settings.reset();
}

namespace {

AppSettings load_app_settings_from_disk() {
  AppSettings settings;

  std::error_code existence_error;
  const auto path = app_settings_file_path();
  if (!std::filesystem::is_regular_file(path, existence_error)) {
    const auto recovery = previous_path(path);
    if (!std::filesystem::is_regular_file(recovery, existence_error)) {
      return settings;
    }
  }

  try {
    Json data;
    try {
      data = read_json_file(path);
      if (!data.is_object()) throw std::runtime_error("Application settings are not an object.");
    } catch (const std::exception &) {
      data = read_json_file(previous_path(path));
      if (!data.is_object()) throw std::runtime_error("Application settings are not an object.");
    }
    settings.onboarding_completed = bool_field(data, "onboardingCompleted", false);
    settings.files_access_decision = files_access_decision_from_string(
        string_field(data, "filesAccessDecision", "not-asked"));
    // Absent on any settings.json written before this field existed - those
    // installs never authorized broader discovery, so the missing-key
    // default (not-authorized) is exactly correct, not a guess.
    settings.broad_project_discovery_authorization = broad_project_discovery_authorization_from_string(
        string_field(data, "broadProjectDiscoveryAuthorization", "not-authorized"));
    settings.device_id = string_field(data, "deviceId", "");
    settings.local_actor_id = string_field(data, "localActorId", "");
    if (data.contains("revisionAuthor") && data.at("revisionAuthor").is_string()) {
      std::string author = data.at("revisionAuthor").get<std::string>();
      while (!author.empty() && std::isspace(static_cast<unsigned char>(author.back()))) author.pop_back();
      while (!author.empty() && std::isspace(static_cast<unsigned char>(author.front()))) author.erase(author.begin());
      if (!author.empty()) settings.revision_author = author;
    }
    if (data.contains("localPerson") && data.at("localPerson").is_object()) {
      const auto &person = data.at("localPerson");
      for (const auto &[key, field] : {std::pair{"displayName", &settings.local_display_name},
                                       std::pair{"organisationName", &settings.local_organisation_name}}) {
        if (!person.contains(key) || !person.at(key).is_string()) continue;
        std::string value = person.at(key).get<std::string>();
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
        if (!value.empty()) *field = std::move(value);
      }
    }
    if (data.contains("userDefaultLegalInstructions") &&
        data.at("userDefaultLegalInstructions").is_string()) {
      settings.user_default_legal_instructions =
          data.at("userDefaultLegalInstructions").get<std::string>();
    }

    if (data.contains("account") && data.at("account").is_object()) {
      const auto &account = data.at("account");
      AccountResume resume;
      resume.organisation_id = string_field(account, "organisationId", "");
      resume.seat_id = string_field(account, "seatId", "");
      if (account.contains("contactEmailMasked") && account.at("contactEmailMasked").is_string()) {
        resume.contact_email_masked = account.at("contactEmailMasked").get<std::string>();
      }
      resume.contact_email_verified = bool_field(account, "contactEmailVerified", false);
      for (const auto &[key, field] : {std::pair{"displayName", &resume.display_name},
                                       std::pair{"organisationName", &resume.organisation_name},
                                       std::pair{"organisationEmail", &resume.organisation_email}}) {
        if (!account.contains(key) || !account.at(key).is_string()) continue;
        std::string value = account.at(key).get<std::string>();
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
        if (!value.empty()) *field = std::move(value);
      }
      if (!resume.organisation_id.empty() && !resume.seat_id.empty()) {
        settings.account = std::move(resume);
      }
    }

    if (data.contains("localNodeSetup") && data.at("localNodeSetup").is_object()) {
      const auto &local = data.at("localNodeSetup");
      auto &setup = settings.local_node_setup;
      const int saved_version = int_field(local, "version", 1);
      setup.completed = bool_field(local, "completed", false);

      if (local.contains("workspaces") && local.at("workspaces").is_array()) {
        for (const auto &workspace : local.at("workspaces")) {
          if (workspace.is_object()) setup.workspaces.push_back(local_workspace_from_json(workspace));
        }
      } else if (saved_version < 2) {
        migrate_v1_workspace(local, setup);
      }
      if (local.contains("activeWorkspaceId") && local.at("activeWorkspaceId").is_string()) {
        setup.active_workspace_id = local.at("activeWorkspaceId").get<std::string>();
      }

      if (local.contains("connectedProviders") && local.at("connectedProviders").is_array()) {
        for (const auto &provider : local.at("connectedProviders")) {
          if (provider.is_object()) setup.connected_providers.push_back(connected_provider_from_json(provider));
        }
      }

      if (local.contains("protectedApiPolicy") && local.at("protectedApiPolicy").is_object()) {
        setup.protected_api_policy = protected_api_policy_from_json(local.at("protectedApiPolicy"));
      } else if (saved_version < 2) {
        migrate_v1_protected_api(local, setup);
      }

      if (local.contains("defaultModel") && local.at("defaultModel").is_string()) {
        setup.default_model = local.at("defaultModel").get<std::string>();
      }

      if (local.contains("roleAssignments") && local.at("roleAssignments").is_array()) {
        for (const auto &assignment : local.at("roleAssignments")) {
          if (auto parsed = role_model_assignment_from_json(assignment)) {
            setup.role_assignments.push_back(std::move(*parsed));
          }
        }
      }

      synchronize_reasoning_role_assignment(setup);

      setup.next_step = node_setup_step_from_string(string_field(local, "nextStep", "workspace"));

      // Absent (a settings.json persisted before Phase 24.1D introduced this
      // field) means "openai_hosted" - the exact pre-existing behavior,
      // never silently switched to the new backend.
      const std::string researchBackend = string_field(local, "researchBackend", "openai_hosted");
      setup.research_backend =
          (researchBackend == "openrouter") ? researchBackend : std::string("openai_hosted");
    }

    if (data.contains("dismissedNoticeIds") && data.at("dismissedNoticeIds").is_array()) {
      for (const auto &id : data.at("dismissedNoticeIds")) {
        if (id.is_string()) {
          settings.dismissed_notice_ids.push_back(id.get<std::string>());
        }
      }
    }
	if (data.contains("projectActionPreferences") && data.at("projectActionPreferences").is_array()) {
		for (const Json &entry : data.at("projectActionPreferences")) {
			if (!entry.is_object()) continue;
			ProjectActionPreference preference;
			preference.project_id = string_field(entry, "projectId", "");
			preference.requested_mode = string_field(entry, "requestedMode", "ask_for_approval");
			if (preference.project_id.empty() == false) {
				settings.project_action_preferences.push_back(std::move(preference));
			}
		}
	}
  } catch (const std::exception &) {
    // Corrupted or unrecognized settings fall back to defaults rather than
    // blocking startup.
    settings = AppSettings{};
  }

  return settings;
}

} // namespace

std::optional<DisplayPerson> display_person(const AppSettings &settings) {
  if (settings.account && settings.account->display_name)
    return DisplayPerson{*settings.account->display_name, settings.account->organisation_name};
  if (settings.local_display_name)
    return DisplayPerson{*settings.local_display_name, settings.local_organisation_name};
  return std::nullopt;
}

std::string tracked_change_author(const AppSettings &settings) {
  const std::optional<DisplayPerson> person = display_person(settings);
  if (!person) return settings.revision_author;
  return person->display_name;
}

void save_app_settings(const AppSettings &settings) {
  // Whatever was kept describes the file this call is about to replace, and
  // every read scope reads it again - from when the write begins until it ends.
  ++settings_saves();
  struct ForgetKept {
    ~ForgetKept() {
      KeptSettings &kept = kept_settings();
      {
        const std::lock_guard<std::mutex> lock(kept.mutex);
        kept.filled = false;
      }
      ++settings_saves();
    }
  } forgetKept;
  const auto directory = app_settings_directory();
  ensure_record_directory(directory);

  Json workspaces_json = Json::array();
  for (const auto &workspace : settings.local_node_setup.workspaces) {
    workspaces_json.push_back(local_workspace_to_json(workspace));
  }

  Json connected_providers_json = Json::array();
  for (const auto &provider : settings.local_node_setup.connected_providers) {
    connected_providers_json.push_back(connected_provider_to_json(provider));
  }

  Json role_assignments_json = Json::array();
  for (const auto &assignment : settings.local_node_setup.role_assignments) {
    role_assignments_json.push_back(role_model_assignment_to_json(assignment));
  }

  Json local_setup_json{
      {"version", settings.local_node_setup.version},
      {"completed", settings.local_node_setup.completed},
      {"workspaces", std::move(workspaces_json)},
      {"activeWorkspaceId", settings.local_node_setup.active_workspace_id.has_value()
                                ? Json(*settings.local_node_setup.active_workspace_id)
                                : Json(nullptr)},
      {"connectedProviders", std::move(connected_providers_json)},
      {"protectedApiPolicy", protected_api_policy_to_json(settings.local_node_setup.protected_api_policy)},
      {"defaultModel", settings.local_node_setup.default_model.has_value()
                            ? Json(*settings.local_node_setup.default_model)
                            : Json(nullptr)},
      {"roleAssignments", std::move(role_assignments_json)},
      {"nextStep", to_string(settings.local_node_setup.next_step)},
      {"researchBackend", settings.local_node_setup.research_backend},
  };

  Json data{
      {"onboardingCompleted", settings.onboarding_completed},
      {"deviceId", settings.device_id},
      {"localActorId", settings.local_actor_id},
      {"filesAccessDecision", to_string(settings.files_access_decision)},
      {"broadProjectDiscoveryAuthorization", to_string(settings.broad_project_discovery_authorization)},
      {"localNodeSetup", std::move(local_setup_json)},
      {"revisionAuthor", settings.revision_author},
      {"userDefaultLegalInstructions",
       settings.user_default_legal_instructions
           ? Json(*settings.user_default_legal_instructions)
           : Json(nullptr)},
      {"dismissedNoticeIds", settings.dismissed_notice_ids},
  };
	Json project_action_preferences = Json::array();
	for (const ProjectActionPreference &preference : settings.project_action_preferences) {
		project_action_preferences.push_back(Json{
			{"projectId", preference.project_id},
			{"requestedMode", preference.requested_mode},
		});
	}
	data["projectActionPreferences"] = std::move(project_action_preferences);
  data["localPerson"] = settings.local_display_name
      ? Json{{"displayName", *settings.local_display_name},
             {"organisationName", settings.local_organisation_name ? Json(*settings.local_organisation_name) : Json(nullptr)}}
      : Json(nullptr);
  if (settings.account.has_value()) {
    data["account"] = Json{
        {"organisationId", settings.account->organisation_id},
        {"seatId", settings.account->seat_id},
        {"contactEmailMasked", settings.account->contact_email_masked
                                   ? Json(*settings.account->contact_email_masked)
                                   : Json(nullptr)},
        {"contactEmailVerified", settings.account->contact_email_verified},
        {"displayName", settings.account->display_name ? Json(*settings.account->display_name) : Json(nullptr)},
        {"organisationName", settings.account->organisation_name ? Json(*settings.account->organisation_name)
                                                                 : Json(nullptr)},
        {"organisationEmail", settings.account->organisation_email ? Json(*settings.account->organisation_email)
                                                                   : Json(nullptr)},
    };
  } else {
    data["account"] = nullptr;
  }

  const auto path = app_settings_file_path();
  // Keys this build does not write survive the save, so a newer build's
  // settings are never stripped by an older one; the format version never
  // goes down.
  data["schemaVersion"] = kSettingsSchemaVersion;
  {
    std::error_code exists_error;
    if (std::filesystem::is_regular_file(path, exists_error)) {
      try {
        const Json existing = read_json_file(path);
        if (existing.is_object()) {
          for (auto entry = existing.begin(); entry != existing.end(); ++entry)
            if (!data.contains(entry.key())) data[entry.key()] = entry.value();
          data["schemaVersion"] = std::max(kSettingsSchemaVersion, int_field(existing, "schemaVersion", 0));
          if (existing.contains("localNodeSetup") && existing.at("localNodeSetup").is_object()) {
            const Json &existing_local = existing.at("localNodeSetup");
            Json &local = data["localNodeSetup"];
            for (auto entry = existing_local.begin(); entry != existing_local.end(); ++entry)
              if (!local.contains(entry.key()) && !retired_local_setup_key(entry.key()))
                local[entry.key()] = entry.value();
            local["version"] = std::max(settings.local_node_setup.version, int_field(existing_local, "version", 0));
          }
        }
      } catch (const std::exception &) {
        // An unreadable current file is quarantined below.
      }
    }
  }
  const std::string text = data.dump(2);
  require_room_for_record(path, text.size());
  const auto temporary_path = unique_temporary_path(path);

  try {
    write_json_file(temporary_path, text);

    std::error_code exists_error;
    if (std::filesystem::is_regular_file(path, exists_error)) {
      // Only promote a parseable current file to the recovery slot. A
      // corrupt current file is retained in place until the new complete
      // settings file atomically replaces it; it must never evict the last
      // known-good recovery copy.
      try {
        (void)read_json_file(path);
        const auto recovery_temporary = unique_temporary_path(previous_path(path));
        try {
          stage_copy_of_file(path, recovery_temporary);
          atomic_replace(recovery_temporary, previous_path(path));
        } catch (...) {
          std::error_code recovery_cleanup_error;
          std::filesystem::remove(recovery_temporary, recovery_cleanup_error);
          throw;
        }
      } catch (const nlohmann::json::exception &) {
        // Keep the existing recovery copy, if any, and keep the unreadable
        // bytes beside it: the replacement below would otherwise destroy the
        // only copy of whatever the file held.
        std::error_code quarantine_error;
        const auto stamp = std::to_string(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        std::filesystem::copy_file(path, path.string() + ".unreadable-" + stamp,
                                   std::filesystem::copy_options::skip_existing, quarantine_error);
      }
    }
    atomic_replace(temporary_path, path);
  } catch (...) {
    std::error_code cleanup_error;
    std::filesystem::remove(temporary_path, cleanup_error);
    throw;
  }
}

nlohmann::json connected_provider_json(const ConnectedProvider &provider) {
  return connected_provider_to_json(provider);
}

ConnectedProvider connected_provider_of(const nlohmann::json &value) {
  return connected_provider_from_json(value);
}

} // namespace singularity::platform
