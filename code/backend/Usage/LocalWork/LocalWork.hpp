#pragma once

#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Provider/Types/UsageMetadata.hpp"

#include <algorithm>
#include <optional>
#include <string>

// Work a model did for a Project outside any chat turn - keeping its memory,
// naming a chat. Usage counts a Project's operations, so such work is recorded
// as an operation of its own: what kind of work it was, which model did it,
// where, and how many tokens it took - never what was read or written. It
// belongs to no chat, so no chat shows it; Usage counts it as local or external
// work like any other.
//
// Header-only: one record appended through the Project's ordinary save.
namespace singularity::usage {

inline constexpr const char *kLocalWorkMemory = "memory_keeping";
inline constexpr const char *kLocalWorkChatNaming = "chat_naming";

struct LocalWork {
	std::string project_id;
	std::string operation_type;
	// What was done, in a few words - the record's only text.
	std::string label;
	std::string model_id;
	// The provider, when the keeper is a provider's model (the model id alone
	// does not say whose it is).
	std::string provider_id;
	// "local", or "external" when the memory's keeper is a provider's model.
	std::string execution_kind = "local";
	long long input_tokens = 0;
	long long output_tokens = 0;
	// The calls' own usage as the provider reported it, its price included when
	// it gave one. Without it a provider's model doing this work is counted in
	// tokens only, and Usage reads a smaller bill than was charged.
	std::optional<providers::UsageMetadata> usage;
	// How Papinian Guard protected the work's provider calls. A record without
	// it read "protection off" for memory a provider's model kept under the
	// Project's protection (24 Sep 2026).
	bool protected_api_applied = false;
	protected_api::papinian::PrivacyMetadata privacy_metadata;
};

// Never throws: a record that cannot be written costs the work nothing.
inline void record_local_work(const LocalWork &work) noexcept {
	try {
		if (work.project_id.empty() || work.model_id.empty() || work.input_tokens + work.output_tokens <= 0) return;
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const auto workspace = node::require_active_workspace_path();
		std::vector<matters::Matter> projects = matters::load_matters(workspace);
		const auto project = std::find_if(projects.begin(), projects.end(),
			[&](const matters::Matter &candidate) { return candidate.id == work.project_id; });
		if (project == projects.end()) return;

		matters::MatterTask task;
		task.id = matters::generate_task_id();
		task.created_at = workspaces::current_timestamp();
		task.operation_type = work.operation_type;
		task.operation_role = "context";
		task.prompt = work.label;
		const bool local = work.execution_kind != "external";
		const std::string provider = local ? std::string("local") : work.provider_id;
		task.requested_provider_id = provider;
		task.requested_model_id = work.model_id;
		task.provider_id = provider;
		task.model_id = work.model_id;
		task.execution_kind = local ? "local" : "external";
		if (local) task.runtime_id = "local";
		task.protected_api_applied = work.protected_api_applied;
		task.privacy_metadata = work.privacy_metadata;
		task.protected_api_mode = work.privacy_metadata.mode;
		if (work.usage) task.usage = *work.usage;
		task.usage.input_tokens = work.input_tokens;
		task.usage.output_tokens = work.output_tokens;
		task.usage.total_tokens = work.input_tokens + work.output_tokens;
		task.status = matters::TaskStatus::Success;
		project->tasks.push_back(std::move(task));
		matters::save_matters(workspace, projects);
	} catch (...) {
	}
}

} // namespace singularity::usage
