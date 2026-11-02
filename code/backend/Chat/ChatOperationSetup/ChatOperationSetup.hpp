#pragma once

#include "Matter/Types/Matter.hpp"
#include "Routing/AiSetup/AiSetup.hpp"
#include "Routing/Types/ModelTarget.hpp"

#include <optional>
#include <string>
#include <vector>

namespace singularity::chat {

// Phase 22's narrow pre-execution resolution boundary
// (phase-22-chat-tasks-ai-setup.md's "Operation setup / one-time
// override"). Input for one prospective Chat operation - never a real send:
// this resolves what a real operation *would* use, exactly the boundary
// Phase 24 execution consumes.
struct OperationSetupRequest {
  std::string project_id;
  std::optional<std::string> chat_id;
  // Empty/absent resolves to LegalTaskPresets' canonical General default
  // (context::kDefaultLegalTaskPresetId/Version) - General is never
  // permanently displayed to the user, only silently applied. A non-empty,
  // unrecognized id is rejected rather than silently substituted with
  // General (LegalTaskPresets.hpp's own contract).
  std::optional<std::string> legal_task_preset_id;
  // Omitted resolves the current (highest) known version for
  // legal_task_preset_id - a caller naming only a task by id, like Chat's
  // own composer, never has to know preset versioning.
  std::optional<int> legal_task_preset_version;
  // Already validated against the live model/provider registry by the
  // caller (Bridge/ChatBridge/ChatBridge.cpp's resolve_operation_setup()) -
  // this domain function never itself touches the local-model/provider
  // runtime, exactly like chat::save_ai_setup_overrides() next to it stays
  // free of that build footprint. Applies only to the resolved
  // primary_result_role below - never any other role.
  std::optional<routing::ModelTarget> one_time_primary_model_override;
};

struct ResolvedOperationRole {
  routing::AiSetupRole role;
  // Unset only when source == Unassigned.
  std::optional<routing::ModelTarget> model;
  routing::AiSetupSource source;
};

struct OperationSetup {
  std::string legal_task_preset_id;
  int legal_task_preset_version = 1;
  std::string legal_task_preset_name;
  // The one role whose model is this operation's user-facing result -
  // Reasoning for ordinary conversational legal work, or a real dedicated
  // stage (currently only Drafting) when the resolved preset's task
  // profile genuinely defines one (Routing/AiSetup/AiSetup.hpp's
  // task_profile_for()/operation_kind_for_legal_task_preset()). Never
  // guessed by a caller from the preset's display name.
  routing::AiSetupRole primary_result_role;
  std::optional<routing::ModelTarget> primary_model;
  routing::AiSetupSource primary_model_source;
  // Every Beta 1 role's effective assignment - Node default -> Project
  // override -> Chat override -> this one optional one-time override
  // (AI_SETUP_CONTRACT.md's "Inheritance"), in
  // routing::all_ai_setup_roles() order. A non-primary role's source is
  // never OperationOverride - the one-time override only ever touches
  // primary_result_role.
  std::vector<ResolvedOperationRole> effective_roles;
  bool one_time_override_applied = false;
};

// Resolves - but never executes - the exact legal task/preset, primary
// result-producing role/model and full effective role assignment set a real
// Phase 24 operation would use. Reuses Routing/AiSetup/AiSetup.hpp's
// resolve_effective_setup() - the same resolver
// Bridge/RoutingBridge/RoutingBridge.cpp's getEffectiveAiSetup already
// reads from - and Routing/LegalTaskPresets/LegalTaskPresets.hpp for the
// canonical task/preset truth. Throws for a Project/Chat that no longer
// exists or an unrecognized legal task preset id/version.
//
// --- "Compare with another model" and "Try again" ------------------------
//
// Both are defined here as setup transformations, and neither is executed
// by Phase 22 (no Chat result surface exists yet, so nothing may render an
// answer/attempt card). Each is the composition of two existing halves:
//
//   setup half (this function): "Compare with another model" is exactly
//   this same request with one_time_primary_model_override set and
//   everything else - Project, Chat, legal task, and therefore every
//   supporting role's effective assignment - byte-for-byte unchanged. That
//   is what makes the comparison controlled: only primary_result_role's
//   model differs, and effective_roles proves it, since a non-primary
//   role's source can never be OperationOverride. "Try again" is the same
//   request with no override at all.
//
//   identity half (already existing, Routing/MatterRequestExecution.hpp's
//   retry_task()): it reuses the original operation's prompt, legal task
//   preset id/version, prompt profile, selected documents, protection mode
//   and Matter instructions snapshot, sets root_task_id/attempt from
//   matters::task_group_id()/next_attempt_number(), and APPENDS a new
//   MatterTask - the previous result is never replaced or deleted.
//
// Phase 24 connects the two against a real result. Nothing here creates,
// names or implies a result that does not exist yet.
OperationSetup resolve_operation_setup(const OperationSetupRequest &request);

} // namespace singularity::chat
