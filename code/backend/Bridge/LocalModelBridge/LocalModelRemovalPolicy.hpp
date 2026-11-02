#pragma once

#include "LocalModel/LocalModelRegistry/LocalModelRegistry.hpp"
#include "Routing/AiSetup/AiSetup.hpp"

#include <string>
#include <vector>

namespace singularity::bridge::local_model_bridge {

// One real assignment (the Node default, a Project override or a Chat
// override) that already references the model being considered for
// removal - the exact real scope, never a synthesized label, so a refusal
// message can name where the assignment actually lives.
struct ModelAssignmentReference {
	routing::AiSetupRole role;
	// "" for the Node default; a human Project name for a Project override;
	// "a Chat in <project name>" for a Chat override - see
	// LocalModelAssignmentScan.cpp, the only real caller that constructs one
	// of these from real Node/Project/Chat truth.
	std::string scope_description;
};

// What a successful uninstall must do beyond deleting the artifact - real
// product/domain truth about this specific model's identity, never an
// implementation detail the orchestrator infers from an incidental
// boolean. See LocalModel/LocalModelRegistry/LocalModelRegistry.hpp's
// unregister_model() for the full architectural contract this encodes.
enum class UninstallDisposition {
	// A package-manager-backed model with a real catalogue/reinstall
	// identity - a successful uninstall deletes only the artifact. The
	// descriptor stays registered: list_status() already re-derives
	// installed/disk_bytes from the live filesystem, so the model
	// correctly reverts to Available/Install with no further mutation.
	PreserveIdentity,
	// A discovered artifact with no catalogue/reinstall identity of its
	// own (LocalInferenceService registered it only because it found the
	// file already on disk, with no matching Install action to ever bring
	// it back) - a successful uninstall deletes the artifact AND forgets
	// its transient registration; leaving a ghost Available/Install card
	// that can never actually work would be dishonest.
	RemoveEphemeralRegistration,
	// Neither of the above - no real backend removal mechanism exists yet
	// for this model. Always refused before any deletion is attempted.
	Unsupported,
};

// Pure policy for Local storage/Manage's "Remove from this computer"
// (Phase 07) - never touches the registry/settings/filesystem/Matter
// storage itself, so this exact refusal sequence is directly testable (see
// LocalModelRemovalPolicy.test.cpp) without linking the local model
// runtime, Provider runtime or Matter persistence at all. The real bridge
// command (LocalModelBridge.cpp's remove(), via
// LocalModelUninstallOrchestrator.hpp) gathers every argument from real
// Node/Project/Chat/registry truth first and then defers entirely to this
// function for the decision, so the delete-consequence check is never
// silently narrower than what this asserts. Throws std::runtime_error with
// the exact user-facing refusal message when removal is not currently
// allowed; returns normally when it is.
//
// `uninspectable_workspace_names` names every registered workspace the
// assignment scan could not actually inspect (moved/unmounted/deleted) -
// a Node-wide destructive uninstall must never interpret "could not check"
// as "nothing found there", so a non-empty list always refuses, even when
// `assigned` is empty.
void check_removal_allowed(const hardware::LocalModelStatus& status,
	UninstallDisposition disposition,
	const std::vector<ModelAssignmentReference>& assigned,
	const std::vector<std::string>& uninspectable_workspace_names);

} // namespace singularity::bridge::local_model_bridge
