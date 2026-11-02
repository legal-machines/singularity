#pragma once

#include "Context/ContextRegistry/ContextRegistry.hpp"
#include "Context/ContextAutomation/ContextAutomation.hpp"
#include "Context/Types/ProjectContext.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"
#include "PapinianGuard/Types/PrivacyMetadata.hpp"

#include <string>
#include <optional>
#include <vector>

namespace singularity::context_actions {

// Resolves a Project id to its Matter, exposing the canonical-versus-
// suggested read/decision surface a Bridge needs - the same "resolve id,
// mutate, save" shape as Privacy/PrivacyActions.hpp.

struct ContextItemView {
	context::ContextItem item;
	context::ContextItemStanding standing;
};

std::vector<ContextItemView> list_items(const std::string &id);
std::vector<context::ContextSuggestion> list_pending_suggestions(const std::string &id);
std::vector<context::ContextSearchResult> search_items(const std::string &id, const std::string &query);
context::ContextPageState get_page_state(const std::string &id);

struct AutomaticContextUpdateResult {
	context::ContextBuildApplication application;
	std::string operation_id;
	std::string provider_id;
	std::string model_id;
	std::string execution_kind;
	std::string ai_setup_source;
	std::string runtime_id;
	std::string runtime_version;
	std::string runtime_source_commit;
	std::string runtime_platform;
	std::string runtime_backend;
	std::string runtime_model_id;
	protected_api::papinian::PrivacyMetadata privacy_metadata;
	bool protected_api_applied = false;
};

// Runs the configured Context AI Setup role over real AI-ready Project
// document chunks, validates structured/source-backed candidates, then
// commits them through ContextCuration in one save. Inference/parsing failure
// occurs before mutation and leaves manual Context fully available.
AutomaticContextUpdateResult run_automatic_update(const std::string &id);

// THE MODEL ASSIGNED TO KEEP THE PROJECT'S MEMORY: the Context role's assignment
// (AI Setup) - a local model or, when the lawyer assigned one, a provider's
// model reached through Papinian Guard like every other request. std::nullopt
// when no Context model is assigned (the application then falls back to a
// local model it has, if any).
struct MemoryModel {
	routing::ModelTarget target;
	// "local" or "external".
	std::string execution_kind;
};
std::optional<MemoryModel> memory_model(const std::string &id);
// The Project's entities for Papinian Guard, for a request made outside a chat.
std::vector<routing::ProjectEntityProtectionHint> memory_protection_hints(const std::string &id);

// Applies a Pending suggestion and returns the affected canonical item.
// Throws if the Project or the suggestion does not exist, or the suggestion
// is no longer Pending.
context::ContextItem accept_suggestion(const std::string &id, const std::string &suggestion_id);

void dismiss_suggestion(const std::string &id, const std::string &suggestion_id);

// Phase 13 (Context - editing, provenance and entities). Direct human
// create/edit/delete on a canonical item, resolving actor identity the same
// way accept_suggestion/dismiss_suggestion above already do. Throws if the
// Project does not exist or the underlying ContextCuration call refuses the
// request (see ContextCuration.hpp for each case).
context::ContextItem create_item(
		const std::string &id, context::ContextSection section, const std::string &label, const std::string &value);

// Phase 28 criterion 40: "Adding a result to Context is a separate explicit
// action." Resolves id/matter_task_id the same way as every other action
// here, then forwards to ContextCuration.hpp's create_item_from_chat_result,
// which enforces the real Chat-result validation (task exists, succeeded,
// not already promoted). Throws under the same conditions that function
// documents.
context::ContextItem create_item_from_chat_result(const std::string &id, context::ContextSection section,
		const std::string &label, const std::string &value, const std::string &matter_task_id);

context::ContextItem update_item_value(const std::string &id, const std::string &item_id,
		const std::string &value, const std::optional<context::ContextItem> &base_item = std::nullopt);
void delete_item(const std::string &id, const std::string &item_id);

// Links/unlinks an item to a Project Entity Registry entry - see
// ContextCuration.hpp's link_item_entity/unlink_item_entity for the
// validation/error cases this forwards.
context::ContextItem link_item_entity(const std::string &id, const std::string &item_id, const std::string &entity_id);
context::ContextItem unlink_item_entity(const std::string &id, const std::string &item_id);
context::ContextItem update_item_controls(const std::string &id, const std::string &item_id,
		context::ContextItemKind kind, bool is_pinned, bool is_locked, bool is_excluded_from_ai);
std::vector<context::ContextMergeConflict> list_merge_conflicts(const std::string &id);
std::optional<context::ContextItem> resolve_merge_conflict(const std::string &id, const std::string &conflict_id,
		const std::string &resolution, const std::optional<std::string> &edited_value);

} // namespace singularity::context_actions
