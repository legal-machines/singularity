#pragma once

#include "Bridge/LocalModelBridge/LocalModelAssignmentScan.hpp"
#include "Bridge/LocalModelBridge/LocalModelRemovalPolicy.hpp"
#include "LocalModel/LocalModelRegistry/LocalModelRegistry.hpp"

#include <functional>

namespace singularity::bridge::local_model_bridge {

// The exact orchestration Local storage/Manage's "Remove from this
// computer" performs (Phase 07): decide via check_removal_allowed(), and
// only if it does not refuse, delete the artifact - then, and only for
// `UninstallDisposition::RemoveEphemeralRegistration`, forget the
// registration too. A `PreserveIdentity` model is never unregistered: its
// descriptor stays exactly as registered, and
// hardware::LocalModelRegistry::list_status() already re-derives
// installed/disk_bytes from the live filesystem, so deleting the artifact
// alone is already enough for it to revert to Available/Install.
//
// `delete_artifact`/`forget_registration` are injected so this exact
// sequencing is directly testable (see
// LocalModelUninstallOrchestrator.test.cpp) against a real
// hardware::LocalModelRegistry and real filesystem operations, without
// linking LocalInferenceService/the local-model runtime at all - the
// real bridge command (LocalModelBridge.cpp's remove()) binds them to
// LocalInferenceService::rediscover_after_package_change()/LocalModelRegistry::unregister_model().
// `delete_artifact` must throw std::runtime_error if the artifact could
// not actually be deleted; this function never swallows that failure, and
// `forget_registration` is never called when it throws.
void uninstall_local_model(const hardware::LocalModelStatus& status,
	UninstallDisposition disposition,
	const AssignmentScanResult& assignment_scan,
	const std::function<void()>& delete_artifact,
	const std::function<void()>& forget_registration);

} // namespace singularity::bridge::local_model_bridge
