#include "Bridge/LocalModelBridge/LocalModelRemovalPolicy.hpp"

#include <cstddef>
#include <stdexcept>

namespace singularity::bridge::local_model_bridge {

void check_removal_allowed(const hardware::LocalModelStatus& status,
	UninstallDisposition disposition,
	const std::vector<ModelAssignmentReference>& assigned,
	const std::vector<std::string>& uninspectable_workspace_names) {

	// A required module (Papinian: core Protected API dependency) is never
	// removable here, matching
	// LocalModelRegistry::release_reclaimable_memory()'s same fail-closed
	// boundary - real product/lifecycle policy for this specific model, not
	// a generic delete path.
	if (status.descriptor.required) {
		throw std::runtime_error(status.descriptor.display_name + " is required and can't be removed here.");
	}

	if (!status.installed) {
		throw std::runtime_error("This local model is not installed.");
	}

	// A model cannot be deleted while it is loaded or actively executing -
	// the user unloads it explicitly first rather than this command silently
	// coordinating a live unload as part of a destructive operation.
	if (status.live.load_state == hardware::LocalModelLoadState::Ready ||
		status.live.load_state == hardware::LocalModelLoadState::Loading) {
		throw std::runtime_error("Unload " + status.descriptor.display_name + " before removing it from this computer.");
	}

	// A Node-wide destructive operation must never interpret "a registered
	// workspace could not be inspected" as "that workspace has no
	// assignments" - fail closed rather than silently under-checking.
	if (!uninspectable_workspace_names.empty()) {
		std::string names;
		for (std::size_t index = 0; index < uninspectable_workspace_names.size(); ++index) {
			if (index > 0) names += ", ";
			names += "\"" + uninspectable_workspace_names[index] + "\"";
		}
		throw std::runtime_error("Singularity can't safely verify whether " + status.descriptor.display_name +
			" is still used by Projects in " + names +
			". Reconnect or restore access to that workspace before removing this model.");
	}

	// The real, backend-enforced delete-consequence check - never trust the
	// frontend alone for this invariant, and never limited to Node-scope
	// assignment: a model assigned only by a Project or Chat override must
	// block exactly the same way. Blocking uniformly for every assigned role
	// (not only Anonymization) is the strictest, safest consequence policy
	// and never silently rewrites AI Setup at any level.
	if (!assigned.empty()) {
		std::string detail;
		for (std::size_t index = 0; index < assigned.size(); ++index) {
			if (index > 0) detail += "; ";
			detail += routing::display_label(assigned[index].role);
			if (!assigned[index].scope_description.empty()) detail += " (" + assigned[index].scope_description + ")";
		}
		throw std::runtime_error(status.descriptor.display_name + " is currently used for " + detail +
			". Choose a replacement before removing it.");
	}

	if (disposition == UninstallDisposition::Unsupported) {
		// No package-manager-backed, non-required local model exists in the
		// product today (Papinian NER is the only package-manager-backed
		// model and it is required, so it is rejected above before ever
		// reaching this point). A future optional catalogue-backed model's
		// uninstall belongs at that model's own storage/package boundary,
		// reporting PreserveIdentity once it exists - never invented here.
		throw std::runtime_error("This local model can't be removed yet.");
	}
}

} // namespace singularity::bridge::local_model_bridge
