#pragma once

#include "LocalModel/LocalModelRegistry/LocalModelRegistry.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Routing/Types/ModelTarget.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace singularity::routing {

// The real Beta 1 AI Setup roles (AI_SETUP_CONTRACT.md). `Gaius`/Triplets is
// reserved future capability, not a Beta 1 role, and is deliberately absent
// here - see all_ai_setup_roles() below, which is the only place a role can
// enter resolution/UI, so a future role is a new enumerator plus a new
// registry entry, never an invented card.
enum class AiSetupRole { Reasoning, Anonymization, SemanticSearch, Context, Drafting };

// Wire/persistence identifiers - lower camelCase, stable across releases
// (native contract + .lm/settings.json values).
const char *to_string(AiSetupRole role);
std::optional<AiSetupRole> ai_setup_role_from_string(const std::string &value);

// The exact product-facing role name (AI_SETUP_CONTRACT.md, phase-06 spec) -
// backend truth so the frontend never re-hardcodes its own copy of these
// names, and so a domain error message below can name a role in plain
// language.
const char *display_label(AiSetupRole role);

// Canonical Beta 1 order, used everywhere a role list is produced so Node
// default AI Setup, Project/Chat overrides and capability metadata all
// enumerate roles the same way.
const std::vector<AiSetupRole> &all_ai_setup_roles();

// One role's assignment at a single inheritance level (Node default,
// Project override, Chat override or a one-time operation override). A
// level that has not customized a role simply omits it here - overrides are
// persisted as explicit differences, never a copied full configuration
// (D-006, AI_SETUP_CONTRACT.md).
struct RoleAssignment {
  AiSetupRole role;
  ModelTarget model;
};

// Only entries a level actually customized - sparse by construction.
using AiSetupOverrideSet = std::vector<RoleAssignment>;

// Where an effective role assignment actually came from. `Automatic` is
// deliberately absent: this resolver only ever performs deterministic
// inheritance, never dynamic routing, so it must never produce a source a
// UI could mistake for real Automatic/"Singularity chooses" routing - see
// AI_SETUP_CONTRACT.md's "Automatic is not inheritance".
enum class AiSetupSource { NodeDefault, ProjectOverride, ChatOverride, OperationOverride, Unassigned };
const char *to_string(AiSetupSource source);

struct ResolvedRoleAssignment {
  AiSetupRole role;
  // Unset only when source == Unassigned - no model exists for this role at
  // any inheritance level.
  std::optional<ModelTarget> model;
  AiSetupSource source;
};

// The Node's own canonical Reasoning assignment - role_assignments' single
// "reasoning" entry (Phase 06, AI_SETUP_CONTRACT.md). This is the one
// implementation the real inference resolution path
// (Routing/InferenceExecutor/InferenceExecutor.cpp's
// resolve_generation_inputs()) and every capability/effective-setup query
// share, so changing the canonical Reasoning assignment always changes what
// a real request with no explicit override actually resolves to -
// LocalNodeSetup::default_model is never independently consulted for this.
std::optional<ModelTarget> node_default_reasoning_target(const platform::LocalNodeSetup &setup);

// The one place effective AI Setup is decided - Node default -> Project
// override -> Chat override -> one-time operation override
// (AI_SETUP_CONTRACT.md's "Inheritance"). Each override set is checked in
// that order and the first one naming `role` wins; a set that never names a
// role is pure inheritance, not a copied value.
ResolvedRoleAssignment resolve_role(AiSetupRole role,
                                    const AiSetupOverrideSet &node_default,
                                    const AiSetupOverrideSet &project_override,
                                    const AiSetupOverrideSet &chat_override,
                                    const AiSetupOverrideSet &operation_override);

// resolve_role() applied to every Beta 1 role, in all_ai_setup_roles() order.
std::vector<ResolvedRoleAssignment> resolve_effective_setup(
    const AiSetupOverrideSet &node_default,
    const AiSetupOverrideSet &project_override,
    const AiSetupOverrideSet &chat_override,
    const AiSetupOverrideSet &operation_override);

// --- Role capability / compatibility metadata ---------------------------
//
// Legal Machines model family semantics: Anonymization is Papinian-family; Semantic search is
// Modestin-family. Reasoning/Context/Drafting are generic text roles with
// no family restriction. `Gaius`/Triplets never appears here - Beta 1 has
// no Triplets role to be compatible with.

// SET-2. Whether a person can actually choose the model that fills this role,
// or whether Singularity itself fills it and an assignment would change nothing.
// Protection is the case that matters: Papinian runs Singularity's own bundled
// detection package, and nothing a person assigns to Anonymization has ever
// altered what protects their data. Offering that choice tells them something
// untrue about their own privacy, which is the one place the product must be
// exactly right - so the role is reported as Singularity's own, with the reason,
// instead of being presented as a setting.
[[nodiscard]] bool is_role_assignable(AiSetupRole role);
// Why a role is not assignable, in the words the product uses. Empty for an
// assignable role.
[[nodiscard]] const char *fixed_role_reason(AiSetupRole role);

// One role's model in a set a person saves: the Node's AI Setup, a Matter's
// or a Chat's. `role` is the wire name.
struct RoleAssignmentEntry {
  std::string role;
  std::string provider_id;
  std::string model_id;
};

// The set kept when a person saves `next` over `previous` (Beta 1 #41).
// Only what the save changes is checked, through `validate` (in the app, the
// live registries: validate_role_model_assignment). An entry kept exactly as
// it was stays as it was, even when its model has since gone away, so one
// stale role never blocks changing or clearing another. A role Singularity
// fills itself (is_role_assignable) is never kept: an old entry for it is
// dropped, and a new one is refused with fixed_role_reason. Throws for a role
// that does not exist and for any changed entry `validate` refuses.
std::vector<RoleAssignmentEntry> accept_assignment_set(const std::vector<RoleAssignmentEntry> &previous,
    const std::vector<RoleAssignmentEntry> &next,
    const std::function<void(AiSetupRole, const ModelTarget &)> &validate);

// The local model families a role is restricted to - empty means no family
// restriction (a generic text role).
std::vector<std::string> compatible_local_families(AiSetupRole role);
bool requires_local_family(AiSetupRole role);

// Whether an installed local model is a valid candidate for `role`.
// Deliberately model-name-blind, like ExecutionPlanner::assess_compatibility:
// a family-restricted role checks the candidate's own declared family; a
// generic text role accepts any real generative architecture but never
// Papinian's own non-generative extraction encoder.
bool is_local_model_compatible(AiSetupRole role, const std::string &family,
                               hardware::ModelArchitectureKind architecture_kind);

// Whether a connected provider's model is a valid candidate for `role`.
// Family-restricted roles (Anonymization, Semantic search) never accept a
// provider model - see PRIVACY_CONTRACT.md/AI_SETUP_CONTRACT.md, protection
// and retrieval families stay local.
bool is_provider_model_compatible(AiSetupRole role, bool text_capable);

// validate_role_model_assignment() - the live-registry-aware counterpart
// that actually looks a candidate up in the installed local-model/connected-
// provider runtime - lives in Routing/AiSetupAssignment/AiSetupAssignment.hpp
// instead of here. This header depends only on Platform/AppSettings (plain
// settings data) and LocalModelRegistry's descriptor types, never on
// InferenceExecutor/ProviderRegistry/PapinianGuardService/ModelLifecycle, so
// Routing/AiSetup/AiSetup.test.cpp can exercise every function above without
// linking the whole local-model/provider runtime.

// --- Task profile ---------------------------------------------------------
//
// Which roles a given kind of AI operation actually needs, and which one of
// those roles produces the user-facing result - AI_SETUP_CONTRACT.md's
// "Primary model and supporting roles". Frontend never infers this; it is
// backend truth needed for correct Retry/"Compare with another model" and
// provenance.

enum class AiOperationKind { ConversationalReasoning, Drafting };
const char *to_string(AiOperationKind kind);

struct TaskProfile {
  // Roles this operation kind always invokes - never a blanket "every
  // supporting role Singularity happens to have configured". A normal
  // conversational request always needs Reasoning; it does not always need
  // Anonymization, Semantic search or Context.
  std::vector<AiSetupRole> required_roles;
  // Roles that MAY participate for this operation kind, depending on real
  // per-request signals this static profile does not itself carry - an
  // active protection policy, explicit source selection, Context being
  // enabled for the request. A role appearing here is never automatically
  // invoked merely because it has a model assigned; the concrete caller
  // (a later phase's Chat/task execution) decides participation from the
  // actual request, not from this list alone.
  std::vector<AiSetupRole> supporting_roles;
  AiSetupRole primary_result_role;
};

const TaskProfile &task_profile_for(AiOperationKind kind);

// Which AiOperationKind a legal task preset id maps to (phase-22-chat-tasks-
// ai-setup.md's "Operation setup / one-time override") - the one place a
// Routing/LegalTaskPresets/LegalTaskPresets.hpp preset id becomes a real
// task profile/primary role. Only "draft" resolves the real dedicated
// drafting_profile() defined above; every other id, including "general" and
// any id this function does not recognize, is ordinary conversational legal
// work - callers that must reject an unrecognized preset id outright do so
// against LegalTaskPresets.hpp's own registry, not by relying on this
// function's fallback.
AiOperationKind operation_kind_for_legal_task_preset(std::string_view legal_task_preset_id);

// --- Assignment queries ----------------------------------------------------
//
// The roles (if any) within one override set whose assignment targets
// exactly `target` - the shared "is this model currently used" query a
// delete-consequence check (Bridge/LocalModelBridge/LocalModelBridge.cpp's
// Local storage uninstall guard) needs at every inheritance level (Node
// default, a Project override, a Chat override), read through the exact
// same AiSetupOverrideSet shape resolve_role() itself reads - never a
// second assignment index. Pure and data-only, like the rest of this
// header: the caller assembles the override set from real
// LocalNodeSetup::role_assignments / Matter::ai_setup_overrides /
// MatterChat::ai_setup_overrides first.
std::vector<AiSetupRole> roles_assigned_to(const AiSetupOverrideSet& overrides, const ModelTarget& target);

} // namespace singularity::routing
