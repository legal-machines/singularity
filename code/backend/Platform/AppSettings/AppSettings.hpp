#pragma once

#include "PapinianGuard/Policy/Policy.hpp"
#include "Provider/ModelClassification/ModelClassification.hpp"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace singularity::platform {

enum class FilesAccessDecision {
	NotAsked,
	Declined,
	Granted,
};

const char *to_string(FilesAccessDecision decision);
FilesAccessDecision files_access_decision_from_string(const std::string &value);

// Whether the user has explicitly authorized broader, system-wide Project
// discovery beyond selected folders (the "Allow broader access" action).
// Deliberately independent of FilesAccessDecision: that field's Granted
// value means only "at least one folder was selected" (see
// Bridge/PlatformBridge's choose_files_and_projects) and must never be read
// as authorizing broad discovery - see PermissionAccess.hpp's
// resolve_project_discovery_scope() for where this is combined with actual,
// live filesystem accessibility.
enum class BroadProjectDiscoveryAuthorization {
	NotAuthorized,
	Authorized,
};

const char *to_string(BroadProjectDiscoveryAuthorization authorization);
BroadProjectDiscoveryAuthorization broad_project_discovery_authorization_from_string(const std::string &value);

// Non-secret identifiers of the seat that most recently completed account
// creation or sign-in on this device. Lets onboarding resume directly at
// Singularity setup (Phase B) after a restart, without asking the user to sign
// in again, once their Legal Machines account already exists. Never holds
// a password or session token - see ProviderSecretStore for the one piece
// of onboarding state that genuinely is a secret.
struct AccountResume {
  std::string organisation_id;
  std::string seat_id;
  // A display-only cache of the Organisation Contact Email's status on the
  // public Legal Machines backend (e.g. "le***@example.com") - never the full
  // address, never a secret. Refreshed whenever onboarding.saveAccountResume
  // is called with fresher values (account creation, sign-in, a contact
  // email change in Settings); stale between those points, same as
  // organisation_id/seat_id themselves. The public backend - not this file
  // - is the source of truth; see entities/organisation-account in the
  // frontend for the API calls that keep it fresh.
  std::optional<std::string> contact_email_masked;
  bool contact_email_verified = false;
  // The firm's own email in full, as its member typed it to sign in: every
  // seat of the firm signs in with it, and the administrator hands it to a
  // colleague with their seat. It is the organisation's role mailbox, never
  // a person's (the licence asks for one).
  std::optional<std::string> organisation_email;
  // The signed-in person as the Legal Machines account names them, and
  // their firm - display-only, refreshed on sign-in like the rest of this.
  std::optional<std::string> display_name;
  std::optional<std::string> organisation_name;
};

// A local folder Singularity works with matters/documents from. Deliberately
// not just a path: `id` is stable across a rename/move (see
// Node/LegacyWorkspace/LegacyWorkspace.hpp's relocate flow), and `display_name`
// lets a moved/renamed folder keep a recognizable identity in the UI even
// while its path is being fixed up.
struct LocalWorkspace {
  std::string id;
  std::string display_name;
  std::string path;
  // ISO 8601 UTC, e.g. "2026-08-19T12:00:00Z".
  std::string created_at;
  std::optional<std::string> last_opened_at;
};

// One provider entry from the authenticated Legal Machines model catalog.
// This structure contains no provider credential or credential status.
struct ConnectedProvider {
  std::string id;
  // Set for "openai_compatible" and for every custom provider connection -
  // see Provider/ProviderRegistry/ProviderRegistry.hpp's own
  // provider_requires_base_url().
  std::optional<std::string> base_url;
  std::vector<providers::ModelDescriptor> models;
  // The user's own chosen label for a custom connection (e.g. "OpenRouter",
  // "My internal proxy") - std::nullopt for the fixed built-in presets
  // ("openai"/"anthropic"), whose display name is a stable, known constant
  // instead (Provider/ProviderRegistry.hpp's own preset list). A universal
  // "connect any endpoint" mechanism still needs a real human label distinct
  // from its internal wire id, which is a caller-generated slug.
  std::optional<std::string> display_name;
};

// Furthest step of Singularity's per-device setup (Phase B) the user has not
// yet completed. Drives resume position after a restart.
enum class NodeSetupStep {
  Workspace,
  Models,
  ProtectedApi,
  Ready,
};

const char *to_string(NodeSetupStep step);
NodeSetupStep node_setup_step_from_string(const std::string &value);

// One Node-default AI Setup role assignment, stored as plain strings so this
// settings type never depends on the Routing domain - `role` is one of
// routing::to_string(AiSetupRole), validated at the Bridge boundary
// (Bridge/NodeBridge/NodeBridge.cpp). Covers all five Beta 1 roles,
// including Reasoning - see LocalNodeSetup::role_assignments below.
struct RoleModelAssignment {
  std::string role;
  std::string provider_id;
  std::string model_id;
};

// Per-device Singularity configuration collected during onboarding's Phase B
// (local workspace, connected model providers, Protected API policy).
// Distinct from AccountResume: the account can already exist on the server
// while this device still has none of this filled in.
//
// `version` exists so a future Singularity release can migrate this shape
// without guessing what an old settings.json meant. Bumped from 1 to 2 when
// the single `workspacePath` string was replaced by `workspaces` - see
// app_settings.cpp's load_app_settings() for the one-time migration that
// keeps existing dev installs' settings.json working.
struct LocalNodeSetup {
  int version = 2;
  bool completed = false;
  std::vector<LocalWorkspace> workspaces;
  std::optional<std::string> active_workspace_id;
  std::vector<ConnectedProvider> connected_providers;
  protected_api::Policy protected_api_policy;
  // Legacy compatibility mirror of role_assignments' own Reasoning entry
  // ("providerId:modelId" - see Routing/Types/ModelTarget.hpp's
  // parse_model_target()). Never independently authoritative and never read
  // by the real inference resolution path
  // (Routing/InferenceExecutor/InferenceExecutor.cpp's
  // resolve_generation_inputs() reads role_assignments directly, via
  // Routing/AiSetup/AiSetup.hpp's node_default_reasoning_target()). Every
  // write path that changes the Node's Reasoning assignment updates both
  // fields together (Bridge/NodeBridge/NodeBridge.cpp's save_local_setup()),
  // and load_app_settings() below migrates a pre-Phase-06 settings.json that
  // only ever set this field into role_assignments' own Reasoning entry.
  // Retained only as a derived projection for any external reader still
  // keyed on this exact field name.
  std::optional<std::string> default_model;
  // Canonical Node default AI Setup for every Beta 1 role (Reasoning,
  // Anonymization, Semantic search, Context, Drafting) - explicit
  // assignments only; a role absent here has no Node default and resolves
  // Unassigned, never a fabricated one (Routing/AiSetup/AiSetup.hpp's
  // resolve_role()).
  std::vector<RoleModelAssignment> role_assignments;
  NodeSetupStep next_step = NodeSetupStep::Workspace;
  // Which ExternalResearch backend production_adapter() (ResearchTools.cpp)
  // constructs - "openai_hosted" (default, unchanged pre-Phase-24.1D
  // behavior) or "openrouter" (OpenRouterExternalResearchAdapter, added so a
  // Project is not left without Singularity web research merely because ONE
  // independently-configured research transport is quota-limited). Never a
  // router across both within one request - see ResearchTools.cpp's own
  // comment: no silent failover, the selected backend is fixed and its
  // identity is recorded truthfully on the resulting MatterTask.
  std::string research_backend = "openai_hosted";
};

// Local user/device authorization for one stable Project identity. This is
// deliberately application settings, never portable Project state: copying a
// Project cannot carry this machine's authority to another user or device.
struct ProjectActionPreference {
	std::string project_id;
	std::string requested_mode = "ask_for_approval";
};

// Local application settings persisted between runs.
struct AppSettings {
  bool onboarding_completed = false;
  FilesAccessDecision files_access_decision = FilesAccessDecision::NotAsked;
  // Never set as a side effect of files_access_decision - only the explicit
  // "Allow broader access" / "Turn off broader access" actions change this.
  BroadProjectDiscoveryAuthorization broad_project_discovery_authorization =
      BroadProjectDiscoveryAuthorization::NotAuthorized;
  // Stable non-secret identity of this installation. Device metadata only:
  // never used as a stand-in for actor attribution.
  std::string device_id;
  // Stable non-secret local-operator identity used to attribute persisted
  // Project mutations when no authenticated Organisation seat is active.
  // Deliberately distinct from device_id: it names the operator, not the
  // machine, so it never gets reused as device metadata or vice versa.
  std::string local_actor_id;
  std::optional<AccountResume> account;
  LocalNodeSetup local_node_setup;
  // Persistent, provider-neutral instructions applied to legal tasks after
  // the Legal Machines system profile and before Matter instructions.
  std::optional<std::string> user_default_legal_instructions;
  // The name Singularity's tracked changes are recorded under in a Word document -
  // what a partner sees beside each revision in Word's review pane. Never
  // empty: an empty author would write changes without revision marks, and
  // that is not something a setting may do silently.
  std::string revision_author = "Singularity";
  // The person who works on this computer and their firm, as they wrote them
  // in Settings - what an installation nobody signed in to shows beside its
  // rail and writes as the author of a Word revision, the way Word's own
  // "User name" does. Presentation only: it names no seat and grants nothing.
  // A signed-in account's own name takes precedence over it.
  std::optional<std::string> local_display_name;
  std::optional<std::string> local_organisation_name;
  // Notification Center / banner notices this device has dismissed (see
  // InProductNotices.h on the public backend). IDs are server-assigned,
  // opaque strings - dismissal is purely local, per device, with no
  // server-side per-seat identity for it to attach to. Not capped: the
  // list only grows by user action (a dismiss click), which in practice
  // stays small.
  std::vector<std::string> dismissed_notice_ids;
	std::vector<ProjectActionPreference> project_action_preferences;
  // The seats that finished onboarding on this computer, each as
  // "organisationId/seatId": identifiers only, kept when one signs out, so a
  // seat that signs in here again goes straight back to work rather than
  // through onboarding once more.
  std::vector<std::string> onboarded_seats;
};

// A seat as onboarded_seats names it.
inline std::string onboarded_seat_key(const std::string &organisation_id, const std::string &seat_id) {
  return organisation_id + "/" + seat_id;
}

// Directory holding persisted application settings. Created lazily by
// save_app_settings(); callers must not assume it already exists.
std::filesystem::path app_settings_directory();

// This computer's own records, which no other computer may share even when the
// same person uses both: its signing key (`Device Key/`), the colleagues'
// computers confirmed here (`Trusted Devices.json`), operations' authority
// (`Operation Authority/`), durable pauses (`Pauses/`) and, on Windows, the
// provider credentials and this computer's Legal Machines session
// (`credentials/`). On macOS and Linux this is the settings folder itself. On
// Windows it is the folder of this computer's application data,
// %LOCALAPPDATA%\<display name> (Platform/AppPaths application_data_directory),
// never the roaming %APPDATA%, which follows a roaming profile to every computer.
// Created lazily, like the settings folder.
std::filesystem::path computer_records_directory();

// Full path to the settings JSON file inside app_settings_directory().
std::filesystem::path app_settings_file_path();

// Loads settings from disk. If the current file is unreadable or invalid,
// first attempts its last known-good `.previous` recovery copy. Returns
// default-constructed AppSettings (onboarding not completed) only when no
// readable settings are available, preserving the original files.
AppSettings load_app_settings();

// The same settings, shared rather than copied: for a caller that only reads
// them, and reads them often (the active workspace, on every Project read).
std::shared_ptr<const AppSettings> load_app_settings_shared();

// One request reads its settings once. While a scope lives on a thread, every
// read of the settings on that thread is the one the scope took at its first
// read - the file is not looked at again - until this process saves settings,
// on any thread: the next read is then fresh from disk. A change another
// process makes is read by the next request. Scopes nest; the outermost one
// counts its reads in the trace ("app_settings.scope").
class AppSettingsReadScope {
public:
  AppSettingsReadScope();
  ~AppSettingsReadScope();
  AppSettingsReadScope(const AppSettingsReadScope &) = delete;
  AppSettingsReadScope &operator=(const AppSettingsReadScope &) = delete;
};

// The name a tracked change is recorded under - what a partner reads beside the
// revision in Word: the signed-in person as the account names them - their name
// alone, as Word writes a reviewer; else the person this computer's Settings name; and
// only where nobody is named at all, the revision_author setting ("Singularity"
// unless changed) - the truthful name of what made them. The same fields give
// ActorIdentity its display name; authority never comes from them.
std::string tracked_change_author(const AppSettings &settings);

// The person shown for this installation: the signed-in account's name and
// firm, else the local person Settings names; nothing when neither is known.
struct DisplayPerson {
  std::string display_name;
  std::optional<std::string> organisation_name;
};
std::optional<DisplayPerson> display_person(const AppSettings &settings);

// Persists settings through a unique temporary file and atomic platform
// replacement, creating the settings directory if needed and retaining the
// prior valid file as `.previous`. Throws std::runtime_error on write,
// flush, close, copy, or replacement failure.
void save_app_settings(const AppSettings &settings);

// As save_app_settings, and the recovery copy then holds these settings too,
// and an unreadable file kept beside them goes: what the settings held before
// stays in no file. For a change that must leave no copy behind, such as
// signing out, after which no file of this computer's still names the
// account.
void save_app_settings_leaving_no_copy(const AppSettings &settings);

// A connected model account as settings.json writes it, and read back: what
// one of the firm's computers hands another with the account's key.
nlohmann::json connected_provider_json(const ConnectedProvider &provider);
ConnectedProvider connected_provider_of(const nlohmann::json &value);

} // namespace singularity::platform
