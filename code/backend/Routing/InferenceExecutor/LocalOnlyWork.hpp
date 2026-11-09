#pragma once

// Whether a request's work runs only on this computer, decided in one place
// where every model call passes: the executor, and the scripted executor the
// tests run on.

#include "Routing/InferenceExecutor/InferenceExecutor.hpp"
#include "Tool/OperationAuthority/OperationAuthority.hpp"

#include <optional>

namespace singularity::routing {

inline constexpr const char *kLocalOnlyWorkNotSent =
	"This work runs only on this computer, so it was not sent to a model outside it. Nothing was sent.";

// This thread's ScopedLocalOnlyWork, or the authority record of the operation
// the request belongs to - named by its tool context, by its
// authority_operation, or by both. Any one that keeps the work here keeps it
// here: a caller can narrow where its work runs, never widen it, whatever model
// it chose.
inline bool is_local_only_work(const InferenceRequest &request) {
	if (ScopedLocalOnlyWork::is_active() == true) return true;
	if (request.tool_execution_context.has_value() == true) {
		const std::optional<tools::OperationAuthority> authority = tools::operation_authority(
			request.tool_execution_context->project_id, request.tool_execution_context->operation_id);
		if (authority.has_value() == true && authority->local_only == true) return true;
	}
	if (request.authority_operation.has_value() == true) {
		const std::optional<tools::OperationAuthority> authority = tools::operation_authority(
			request.authority_operation->project_id, request.authority_operation->operation_id);
		if (authority.has_value() == true && authority->local_only == true) return true;
	}
	return false;
}

} // namespace singularity::routing
