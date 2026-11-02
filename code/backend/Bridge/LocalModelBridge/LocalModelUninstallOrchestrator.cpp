#include "Bridge/LocalModelBridge/LocalModelUninstallOrchestrator.hpp"

namespace singularity::bridge::local_model_bridge {

void uninstall_local_model(const hardware::LocalModelStatus& status,
	UninstallDisposition disposition,
	const AssignmentScanResult& assignment_scan,
	const std::function<void()>& delete_artifact,
	const std::function<void()>& forget_registration) {

	check_removal_allowed(status, disposition, assignment_scan.references, assignment_scan.uninspectable_workspace_names);
	// check_removal_allowed() throws (and returns before reaching this line)
	// for every refusal case, including Unsupported - so reaching here
	// always means real deletion may proceed for a known, supported model.

	// delete_artifact() throws on a real deletion failure, which unwinds out
	// of this function before forget_registration() ever runs - state is
	// never marked as if uninstall succeeded when the artifact is still on
	// disk.
	delete_artifact();

	// A PreserveIdentity model is never unregistered: the registry's own
	// live filesystem check already makes it Available/Install again once
	// the artifact above is actually gone.
	if (disposition == UninstallDisposition::RemoveEphemeralRegistration) {
		forget_registration();
	}
}

} // namespace singularity::bridge::local_model_bridge
