#include "Bridge/NodeBridge/NodeBridge.hpp"

#include "Bridge/ProviderBridge/ProviderBridge.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegalMachinesConnection/LegalMachinesConnection.hpp"
#include "Node/NodeSummary/NodeSummary.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "PapinianGuard/Policy/Policy.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Routing/AiSetup/AiSetup.hpp"
#include "Routing/AiSetupAssignment/AiSetupAssignment.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

namespace singularity::bridge::node_bridge {

using Json = nlohmann::json;

std::string trim(const std::string &value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

Json workspace_json(const platform::LocalWorkspace &workspace) {
  return Json{
      {"id", workspace.id},
      {"displayName", workspace.display_name},
      {"path", workspace.path},
      {"createdAt", workspace.created_at},
      {"lastOpenedAt", workspace.last_opened_at ? Json(*workspace.last_opened_at) : Json(nullptr)},
      // Recomputed on every read rather than cached - a folder can vanish
      // (moved, unmounted, deleted) at any time between launches, and this
      // is what lets the UI say "no longer available" instead of silently
      // routing to a dead path.
      {"available", workspaces::directory_available(workspace.path)},
  };
}

Json workspace_list_json(const platform::LocalNodeSetup &setup) {
  Json list = Json::array();
  for (const auto &workspace : setup.workspaces) {
    list.push_back(workspace_json(workspace));
  }
  return Json{
      {"workspaces", std::move(list)},
      {"activeWorkspaceId",
       setup.active_workspace_id ? Json(*setup.active_workspace_id) : Json(nullptr)},
  };
}

Json active_workspace_summary_json(const platform::LocalNodeSetup &setup) {
  if (!setup.active_workspace_id) return nullptr;
  const auto found = std::find_if(setup.workspaces.begin(), setup.workspaces.end(),
                                  [&](const platform::LocalWorkspace &workspace) {
                                    return workspace.id == *setup.active_workspace_id;
                                  });
  return found == setup.workspaces.end() ? Json(nullptr) : workspace_json(*found);
}

Json protected_api_policy_json(const protected_api::Policy &policy) {
  const auto effective = protected_api::effective_categories(policy);
  Json categories = Json::array();
  for (const auto category : protected_api::all_categories()) {
    const bool enabled =
        std::find(effective.begin(), effective.end(), category) != effective.end();
    categories.push_back(Json{{"id", protected_api::to_string(category)}, {"enabled", enabled}});
  }
  return Json{
      {"mode", protected_api::to_string(policy.mode)},
      {"categories", std::move(categories)},
      {"customTerms", policy.custom_terms},
  };
}

Json role_assignment_json(const platform::RoleModelAssignment &assignment) {
  return Json{{"role", assignment.role}, {"providerId", assignment.provider_id}, {"modelId", assignment.model_id}};
}

Json local_setup_json(const platform::LocalNodeSetup &setup) {
  Json role_assignments = Json::array();
  for (const auto &assignment : setup.role_assignments) role_assignments.push_back(role_assignment_json(assignment));
  return Json{
      {"version", setup.version},
      {"completed", setup.completed},
      {"activeWorkspace", active_workspace_summary_json(setup)},
      {"connectedProviders", provider_bridge::configured_for_onboarding()},
      {"protectedApiPolicy", protected_api_policy_json(setup.protected_api_policy)},
      {"defaultModel", setup.default_model ? Json(*setup.default_model) : Json(nullptr)},
      {"roleAssignments", std::move(role_assignments)},
      {"nextStep", platform::to_string(setup.next_step)},
  };
}

Json account_resume_json(const platform::AccountResume &account) {
  return Json{
      {"organisationId", account.organisation_id},
      {"seatId", account.seat_id},
      {"contactEmailMasked", account.contact_email_masked ? Json(*account.contact_email_masked) : Json(nullptr)},
      {"contactEmailVerified", account.contact_email_verified},
      {"displayName", account.display_name ? Json(*account.display_name) : Json(nullptr)},
      {"organisationName", account.organisation_name ? Json(*account.organisation_name) : Json(nullptr)},
  };
}

Json display_person_json(const platform::AppSettings &settings) {
  const std::optional<platform::DisplayPerson> person = platform::display_person(settings);
  if (!person) return Json(nullptr);
  return Json{{"displayName", person->display_name},
              {"organisationName", person->organisation_name ? Json(*person->organisation_name) : Json(nullptr)},
              {"source", settings.account && settings.account->display_name ? "account" : "local"}};
}

Json onboarding_status_json(const platform::AppSettings &settings) {
  Json result{
      {"completed", settings.onboarding_completed},
      {"localSetup", local_setup_json(settings.local_node_setup)},
  };
  result["account"] = settings.account.has_value() ? account_resume_json(*settings.account) : Json(nullptr);
  // The person this installation shows: the account's, else the one Settings names.
  result["person"] = display_person_json(settings);
  return result;
}

// A patch, not a full replace, for the contact-status fields: callers pass
// only what changed (e.g. sign-in only refreshes organisationId/seatId;
// completing contact verification only flips contactEmailVerified) -
// anything omitted keeps its previously cached value, same convention as
// onboarding.saveLocalSetup.
Json save_account_resume(const Json &params) {
  const auto organisation_id = params.at("organisationId").get<std::string>();
  const auto seat_id = params.at("seatId").get<std::string>();
  if (organisation_id.empty() || seat_id.empty()) {
    throw std::runtime_error("organisationId and seatId are required");
  }

  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  auto settings = platform::load_app_settings();
  platform::AccountResume resume;
  resume.organisation_id = organisation_id;
  resume.seat_id = seat_id;

  if (settings.account && settings.account->organisation_id == organisation_id) {
    resume.contact_email_masked = settings.account->contact_email_masked;
    resume.contact_email_verified = settings.account->contact_email_verified;
    resume.display_name = settings.account->display_name;
    resume.organisation_name = settings.account->organisation_name;
  }
  if (params.contains("contactEmailMasked")) {
    const auto &value = params.at("contactEmailMasked");
    resume.contact_email_masked =
        value.is_null() ? std::optional<std::string>() : std::optional<std::string>(value.get<std::string>());
  }
  if (params.contains("contactEmailVerified")) {
    resume.contact_email_verified = params.at("contactEmailVerified").get<bool>();
  }
  for (const auto &[key, field] : {std::pair{"displayName", &resume.display_name},
                                   std::pair{"organisationName", &resume.organisation_name}}) {
    if (!params.contains(key)) continue;
    const auto &value = params.at(key);
    *field = value.is_string() && !value.get<std::string>().empty() ? std::optional<std::string>(value.get<std::string>())
                                                                    : std::nullopt;
  }

  settings.account = resume;
  platform::save_app_settings(settings);

  return account_resume_json(resume);
}

Json dismissed_notice_ids_json(const platform::AppSettings &settings) {
  return Json{{"dismissedNoticeIds", settings.dismissed_notice_ids}};
}

Json legal_instruction_settings_json(const platform::AppSettings &settings) {
  return Json{{"userDefaultLegalInstructions",
               settings.user_default_legal_instructions
                   ? Json(*settings.user_default_legal_instructions)
                   : Json(nullptr)}};
}

Json save_legal_instruction_settings(const Json &params) {
  if (!params.contains("userDefaultLegalInstructions") ||
      (!params.at("userDefaultLegalInstructions").is_null() &&
       !params.at("userDefaultLegalInstructions").is_string())) {
    throw std::runtime_error(
        "User default legal instructions must be text or null.");
  }
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  auto settings = platform::load_app_settings();
  if (params.at("userDefaultLegalInstructions").is_null()) {
    settings.user_default_legal_instructions = std::nullopt;
  } else {
    const auto instructions =
        trim(params.at("userDefaultLegalInstructions").get<std::string>());
    settings.user_default_legal_instructions =
        instructions.empty() ? std::nullopt
                             : std::optional<std::string>(instructions);
  }
  platform::save_app_settings(settings);
  return legal_instruction_settings_json(settings);
}

// Notification Center / banner dismissal - purely local, see
// AppSettings::dismissed_notice_ids. Idempotent: dismissing an
// already-dismissed id is a no-op, not a duplicate entry.
Json dismiss_notice(const Json &params) {
  const auto notice_id = params.at("noticeId").get<std::string>();
  if (notice_id.empty()) {
    throw std::runtime_error("noticeId is required");
  }

  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  auto settings = platform::load_app_settings();
  auto &ids = settings.dismissed_notice_ids;
  if (std::find(ids.begin(), ids.end(), notice_id) == ids.end()) {
    ids.push_back(notice_id);
    platform::save_app_settings(settings);
  }
  return dismissed_notice_ids_json(settings);
}

// Replaces (or clears) exactly one role's entry in setup.role_assignments -
// role_assignments is the single canonical Node default AI Setup store for
// every Beta 1 role, including Reasoning (Phase 06, AI_SETUP_CONTRACT.md);
// there must never be two independently writable persisted sources of truth
// for Reasoning. Both the legacy `defaultModel` patch and the canonical
// `roleAssignments` patch below funnel every Reasoning write through this
// one function.
void set_role_assignment(platform::LocalNodeSetup &setup, const std::string &role,
                         std::optional<platform::RoleModelAssignment> assignment) {
  auto &assignments = setup.role_assignments;
  assignments.erase(std::remove_if(assignments.begin(), assignments.end(),
                                   [&](const platform::RoleModelAssignment &entry) { return entry.role == role; }),
                    assignments.end());
  if (assignment) assignments.push_back(std::move(*assignment));
}

Json save_local_setup(const Json &params) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  auto settings = platform::load_app_settings();
  auto &setup = settings.local_node_setup;

  // Legacy single-model patch - kept only for a caller that has not moved
  // to roleAssignments yet. Merges into role_assignments' own Reasoning
  // entry rather than writing setup.default_model directly, so Reasoning is
  // never independently authoritative in two places; if roleAssignments is
  // also present below, it is the newer, canonical patch and wins.
  if (params.contains("defaultModel")) {
    const auto &value = params.at("defaultModel");
    if (value.is_null()) {
      set_role_assignment(setup, "reasoning", std::nullopt);
    } else {
      const auto target = routing::parse_model_target(value.get<std::string>());
      if (!target) throw std::runtime_error("This is not a real model selection.");
      routing::validate_role_model_assignment(routing::AiSetupRole::Reasoning, *target);
      set_role_assignment(setup, "reasoning",
                          platform::RoleModelAssignment{"reasoning", target->provider_id, target->model_id});
    }
  }

  // Canonical Node default AI Setup for every Beta 1 role, including
  // Reasoning. Replaces the full set - it is itself the caller's complete
  // desired assignment list, so omitting a role here is exactly "no Node
  // default for that role" (Unassigned), never a delete of anything else.
  // Each entry this save changes is validated against Routing::AiSetup's
  // real role-compatibility rules before it is saved; one kept as it was is
  // not checked again (Beta 1 #41, routing::accept_assignment_set).
  if (params.contains("roleAssignments")) {
    std::vector<routing::RoleAssignmentEntry> previous;
    for (const platform::RoleModelAssignment &kept : setup.role_assignments)
      previous.push_back({kept.role, kept.provider_id, kept.model_id});
    std::vector<routing::RoleAssignmentEntry> next;
    for (const auto &entry : params.at("roleAssignments"))
      next.push_back({entry.at("role").get<std::string>(), entry.at("providerId").get<std::string>(),
                      entry.at("modelId").get<std::string>()});
    std::vector<platform::RoleModelAssignment> role_assignments;
    for (const routing::RoleAssignmentEntry &accepted :
         routing::accept_assignment_set(previous, next, routing::validate_role_model_assignment))
      role_assignments.push_back(platform::RoleModelAssignment{accepted.role, accepted.provider_id, accepted.model_id});
    setup.role_assignments = std::move(role_assignments);
  }

  // default_model stays a derived mirror of role_assignments' own Reasoning
  // entry, recomputed here after every mutation above via the same
  // node_default_reasoning_target() the real inference resolution path
  // reads (Routing/InferenceExecutor/InferenceExecutor.cpp) - never
  // independently authoritative.
  const auto reasoning_target = routing::node_default_reasoning_target(setup);
  setup.default_model = reasoning_target ? std::optional<std::string>(routing::format_model_target(*reasoning_target))
                                         : std::optional<std::string>();

  if (params.contains("nextStep")) {
    setup.next_step = platform::node_setup_step_from_string(params.at("nextStep").get<std::string>());
  }

  platform::save_app_settings(settings);
  return local_setup_json(setup);
}

// --- Workspaces --------------------------------------------------------

Json list_workspaces_state() {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  return workspace_list_json(platform::load_app_settings().local_node_setup);
}

// Adds `path` as a workspace (or, if it is already one, just re-activates
// it) and makes it the active workspace. Shared by workspace.choose and
// workspace.relocate's "pick a new folder" half.
Json add_or_activate_workspace(platform::AppSettings &settings, const std::filesystem::path &path) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  workspaces::validate_directory(path);
  const auto canonical_wide = std::filesystem::weakly_canonical(path).u8string();
  const std::string canonical(canonical_wide.begin(), canonical_wide.end());

  auto &setup = settings.local_node_setup;
  const auto existing = std::find_if(setup.workspaces.begin(), setup.workspaces.end(),
                                     [&](const platform::LocalWorkspace &workspace) {
                                       return workspace.path == canonical;
                                     });

  std::string workspace_id;
  if (existing != setup.workspaces.end()) {
    existing->last_opened_at = workspaces::current_timestamp();
    workspace_id = existing->id;
  } else {
    platform::LocalWorkspace workspace;
    workspace.id = workspaces::generate_workspace_id();
    workspace.display_name = workspaces::derive_display_name(path);
    workspace.path = canonical;
    workspace.created_at = workspaces::current_timestamp();
    workspace_id = workspace.id;
    setup.workspaces.push_back(workspace);
  }
  setup.active_workspace_id = workspace_id;

  platform::save_app_settings(settings);
  return workspace_list_json(setup);
}

Json remove_workspace(const Json &params) {
  const auto id = params.at("id").get<std::string>();

  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  auto settings = platform::load_app_settings();
  auto &setup = settings.local_node_setup;
  setup.workspaces.erase(
      std::remove_if(setup.workspaces.begin(), setup.workspaces.end(),
                     [&](const platform::LocalWorkspace &workspace) { return workspace.id == id; }),
      setup.workspaces.end());

  if (setup.active_workspace_id == id) {
    setup.active_workspace_id =
        setup.workspaces.empty() ? std::nullopt : std::optional<std::string>(setup.workspaces.front().id);
  }

  platform::save_app_settings(settings);
  return workspace_list_json(setup);
}

Json set_active_workspace(const Json &params) {
  const auto id = params.at("id").get<std::string>();

  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  auto settings = platform::load_app_settings();
  auto &setup = settings.local_node_setup;
  const auto found = std::find_if(
      setup.workspaces.begin(), setup.workspaces.end(),
      [&](const platform::LocalWorkspace &workspace) { return workspace.id == id; });
  if (found == setup.workspaces.end()) {
    throw std::runtime_error("This workspace is no longer configured.");
  }
  if (!workspaces::directory_available(found->path)) {
    throw std::runtime_error("\"" + found->display_name +
                             "\" is no longer available. Locate it or remove it.");
  }

  found->last_opened_at = workspaces::current_timestamp();
  setup.active_workspace_id = id;

  platform::save_app_settings(settings);
  return workspace_list_json(setup);
}

nlohmann::json node_summary_json(const node::NodeSummary &summary) {
  using Json = nlohmann::json;
  return Json{
      {"deviceName", summary.device_name ? Json(*summary.device_name) : Json(nullptr)},
      {"operatingSystem", summary.operating_system},
      {"operatingSystemIdentity", summary.operating_system_identity},
      {"hardwareModel", summary.hardware_model},
      {"memoryPressure", summary.memory_pressure},
      {"cpu", {{"identity", summary.cpu.identity ? Json(*summary.cpu.identity) : Json(nullptr)},
               {"capabilities", summary.cpu.capabilities},
               {"physicalCores", summary.cpu.physical_cores ? Json(*summary.cpu.physical_cores) : Json(nullptr)},
               {"logicalCores", summary.cpu.logical_cores ? Json(*summary.cpu.logical_cores) : Json(nullptr)},
               {"totalMemoryBytes", summary.cpu.total_memory_bytes ? Json(*summary.cpu.total_memory_bytes) : Json(nullptr)},
               {"availableMemoryBytes", summary.cpu.available_memory_bytes ? Json(*summary.cpu.available_memory_bytes) : Json(nullptr)}}},
      {"accelerator", {{"identity", summary.accelerator.identity ? Json(*summary.accelerator.identity) : Json(nullptr)},
                       {"backend", summary.accelerator.backend ? Json(*summary.accelerator.backend) : Json(nullptr)},
                       {"memoryBytes", summary.accelerator.memory_bytes ? Json(*summary.accelerator.memory_bytes) : Json(nullptr)},
                       {"memoryBudgetBytes", summary.accelerator.memory_budget_bytes ? Json(*summary.accelerator.memory_budget_bytes) : Json(nullptr)},
                       {"unifiedMemory", summary.accelerator.unified_memory}}},
      {"availableBackends", summary.available_backends},
      {"runtimeIdentities", summary.runtime_identities},
      {"calibration", {{"memoryBandwidthBytesPerSecond", summary.calibration.memory_bandwidth_bytes_per_second ? Json(*summary.calibration.memory_bandwidth_bytes_per_second) : Json(nullptr)},
                       {"storageReadBytesPerSecond", summary.calibration.storage_read_bytes_per_second ? Json(*summary.calibration.storage_read_bytes_per_second) : Json(nullptr)},
                       {"cpuExecutionGigaOperationsPerSecond", summary.calibration.cpu_execution_giga_operations_per_second ? Json(*summary.calibration.cpu_execution_giga_operations_per_second) : Json(nullptr)},
                       {"acceleratorMemoryBandwidthBytesPerSecond", summary.calibration.accelerator_memory_bandwidth_bytes_per_second ? Json(*summary.calibration.accelerator_memory_bandwidth_bytes_per_second) : Json(nullptr)},
                       {"acceleratorExecutionGigaOperationsPerSecond", summary.calibration.accelerator_execution_giga_operations_per_second ? Json(*summary.calibration.accelerator_execution_giga_operations_per_second) : Json(nullptr)},
                       {"acceleratorCommandOverheadMicroseconds", summary.calibration.accelerator_command_overhead_microseconds ? Json(*summary.calibration.accelerator_command_overhead_microseconds) : Json(nullptr)},
                       {"hostToAcceleratorBandwidthBytesPerSecond", summary.calibration.host_to_accelerator_bandwidth_bytes_per_second ? Json(*summary.calibration.host_to_accelerator_bandwidth_bytes_per_second) : Json(nullptr)},
                       {"hostToAcceleratorSemantics", summary.calibration.host_to_accelerator_semantics},
                       {"acceleratorCalibrationUnavailableReason", summary.calibration.accelerator_calibration_unavailable_reason},
                       {"cpuThroughputScore", summary.calibration.cpu_throughput_score},
                       {"measuredAtEpochSeconds", summary.calibration.measured_at_epoch_seconds}}},
      {"storage", {{"totalBytes", summary.storage.total_bytes},
                   {"localModelsBytes", summary.storage.local_models_bytes},
                   {"matterDocumentsBytes", summary.storage.matter_documents_bytes},
                   {"matterStateBytes", summary.storage.matter_state_bytes},
                   {"indexesCacheBytes", summary.storage.indexes_cache_bytes}}},
      {"loadedLocalModelIds", summary.loaded_local_model_ids},
  };
}

nlohmann::json get_summary(const nlohmann::json &) {
  return node_summary_json(node::get_node_summary());
}

// Whether Legal Machines can be reached, for the startup screen's first line.
// Bounded by its own short timeout and never an error: offline is an ordinary
// state, and Singularity opens either way.
nlohmann::json check_legal_machines_connection(const nlohmann::json &) {
  const node::LegalMachinesConnection connection = node::check_legal_machines_connection();
  return nlohmann::json{
      {"reachable", connection.reachable},
      {"state", connection.state},
      {"elapsedMs", connection.elapsed_ms},
      {"detail", connection.detail},
  };
}

nlohmann::json release_reclaimable_memory(const nlohmann::json &) {
  routing::production_local_model_registry().release_reclaimable_memory();
  return node_summary_json(node::get_node_summary());
}

nlohmann::json get_onboarding_status(const nlohmann::json &) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  return onboarding_status_json(platform::load_app_settings());
}

nlohmann::json complete_onboarding(const nlohmann::json &) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  auto settings = platform::load_app_settings();
  settings.onboarding_completed = true;
  settings.local_node_setup.completed = true;
  settings.local_node_setup.next_step = platform::NodeSetupStep::Ready;
  platform::save_app_settings(settings);
  return onboarding_status_json(settings);
}

// The name and firm of the person who works on this computer, written in
// Settings. An empty name clears it; the firm without a name means nothing.
// A member signed in is known here by their account (Platform/ActorIdentity),
// and Legal Machines names no person for an organisation's seat: the name they
// give is their account's on this computer too, so their member record in
// every Matter and every message they write carry it (onboarding's first page,
// D-134). A later sign-in to the same seat keeps it (AccountBridge
// save_account); one that names the person replaces it.
nlohmann::json save_person(const nlohmann::json &params) {
  const auto text = [&](const char *key) -> std::optional<std::string> {
    if (!params.contains(key) || !params.at(key).is_string()) return std::nullopt;
    std::string value = params.at(key).get<std::string>();
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    if (value.size() > 120) throw std::runtime_error("A name is at most 120 characters.");
    return value.empty() ? std::nullopt : std::optional<std::string>(value);
  };
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  auto settings = platform::load_app_settings();
  settings.local_display_name = text("displayName");
  settings.local_organisation_name = settings.local_display_name ? text("organisationName") : std::nullopt;
  if (settings.account.has_value() == true && settings.local_display_name.has_value() == true) {
    settings.account->display_name = settings.local_display_name;
  }
  platform::save_app_settings(settings);
  return Json{{"person", display_person_json(settings)}};
}

nlohmann::json get_legal_instructions(const nlohmann::json &) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  return legal_instruction_settings_json(platform::load_app_settings());
}

nlohmann::json get_dismissed_notice_ids(const nlohmann::json &) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  return dismissed_notice_ids_json(platform::load_app_settings());
}

nlohmann::json list_workspaces(const nlohmann::json &) { return list_workspaces_state(); }

nlohmann::json choose_workspace(const std::optional<std::string> &selected_path) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  auto settings = platform::load_app_settings();
  if (!selected_path) return workspace_list_json(settings.local_node_setup);
  return add_or_activate_workspace(settings, std::filesystem::path(*selected_path));
}

nlohmann::json grant_folder_access(const std::string &selected_path) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  node::grant_folder_access(std::filesystem::path(selected_path));
  return workspace_list_json(platform::load_app_settings().local_node_setup);
}

void validate_relocation(const nlohmann::json &params) {
  const auto id = params.at("id").get<std::string>();
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto settings = platform::load_app_settings();
  const auto &workspaces = settings.local_node_setup.workspaces;
  if (std::none_of(workspaces.begin(), workspaces.end(),
                   [&](const platform::LocalWorkspace &workspace) { return workspace.id == id; })) {
    throw std::runtime_error("This workspace is no longer configured.");
  }
}

nlohmann::json relocate_workspace(const nlohmann::json &params,
                                  const std::optional<std::string> &selected_path) {
  const auto id = params.at("id").get<std::string>();
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  auto settings = platform::load_app_settings();
  auto &setup = settings.local_node_setup;
  const auto found = std::find_if(setup.workspaces.begin(), setup.workspaces.end(),
                                  [&](const platform::LocalWorkspace &workspace) {
                                    return workspace.id == id;
                                  });
  if (found == setup.workspaces.end())
    throw std::runtime_error("This workspace is no longer configured.");
  if (!selected_path) return workspace_list_json(setup);

  const std::filesystem::path path(*selected_path);
  workspaces::validate_directory(path);
  const auto canonical_wide = std::filesystem::weakly_canonical(path).u8string();
  found->path = std::string(canonical_wide.begin(), canonical_wide.end());
  found->display_name = workspaces::derive_display_name(path);
  found->last_opened_at = workspaces::current_timestamp();
  platform::save_app_settings(settings);
  return workspace_list_json(setup);
}

} // namespace singularity::bridge::node_bridge
