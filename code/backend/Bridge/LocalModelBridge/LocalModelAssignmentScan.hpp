#pragma once

#include "Bridge/LocalModelBridge/LocalModelRemovalPolicy.hpp"
#include "Matter/Types/Matter.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Routing/Types/ModelTarget.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace singularity::bridge::local_model_bridge {

// The result of scanning every persisted scope that can reference a local
// model. `uninspectable_workspace_names` is not an incidental detail: a
// Node-wide destructive uninstall must treat it as fail-closed evidence
// that the scan is INCOMPLETE, never as "those workspaces have no
// assignments" - see check_removal_allowed()'s own contract, which refuses
// whenever this is non-empty regardless of what `references` contains.
struct AssignmentScanResult {
	std::vector<ModelAssignmentReference> references;
	// Human display names of every registered workspace this scan could
	// not actually inspect (moved/unmounted/deleted).
	std::vector<std::string> uninspectable_workspace_names;
};

// Gathers every real assignment reference to `target` across every scope
// that can persist one: the Node default (setup.role_assignments) and,
// for EVERY workspace this Node knows about (setup.workspaces) - not only
// setup.active_workspace_id - every Project's own override and every
// Chat's own override within every Project. A local model is Node-wide,
// shared across every workspace; a Project inside a workspace the user
// simply has not opened this session still has a real, persisted
// assignment that must block removal exactly the same way as one in the
// active workspace - see Platform/PermissionAccess/PermissionAccess.cpp's
// own precedent for scanning every registered workspace, not only the
// active one.
//
// A registered workspace that is not currently reachable is recorded in
// the result's `uninspectable_workspace_names`, never silently skipped as
// if it had nothing to report - "cannot verify" is not "verified empty".
// The active workspace carries no special dependency semantics here: it
// is scanned by the exact same rule as every other registered workspace.
//
// `load_matters_for_workspace`/`is_workspace_available` are injected so
// this exact scanning contract is directly testable (see
// LocalModelAssignmentScan.test.cpp) with in-memory Matter fixtures -
// never real Matter/.singularity-workspace file I/O in a test. The real bridge command
// (LocalModelBridge.cpp's remove()) binds them to
// matters::load_matters()/workspaces::directory_available().
AssignmentScanResult gather_assignment_references(
	const platform::LocalNodeSetup& setup,
	const routing::ModelTarget& target,
	const std::function<std::vector<matters::Matter>(const std::filesystem::path&)>& load_matters_for_workspace,
	const std::function<bool(const std::filesystem::path&)>& is_workspace_available);

} // namespace singularity::bridge::local_model_bridge
