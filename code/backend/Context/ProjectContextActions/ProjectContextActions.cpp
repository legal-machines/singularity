#include "ExternalResearch/ResearchToolLoop/ResearchToolLoop.hpp"
#include "Context/ProjectContextActions/ProjectContextActions.hpp"

#include "Context/ContextCuration/ContextCuration.hpp"
#include "Context/ContextAutomation/ContextAutomation.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Persistence/ProjectStore/ProjectStore.hpp"
#include "Privacy/EntityRegistry/EntityRegistry.hpp"
#include "Privacy/EntityResolver/EntityResolver.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Routing/AiSetup/AiSetup.hpp"
#include "Routing/AiSetupAssignment/AiSetupAssignment.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace singularity::context_actions {
namespace {

matters::Matter require_matter(const std::string &id) {
	auto found = matters::load_matter(node::require_active_workspace_path(), id);
	if (!found) throw std::runtime_error("This project no longer exists.");
	return std::move(*found);
}

long long count_context_conflicts(const matters::Matter &matter) {
	if (matter.directory_path.has_value() == false || persistence::has_project_data(*matter.directory_path) == false) {
		return 0;
	}
	const persistence::ProjectRecord record = persistence::load_project(*matter.directory_path);
	return static_cast<long long>(std::count_if(record.conflicts.begin(), record.conflicts.end(),
			[](const persistence::ProjectConflict &conflict) { return conflict.collection == "contextItems"; }));
}

routing::AiSetupOverrideSet node_default_overrides(const platform::LocalNodeSetup &setup) {
	routing::AiSetupOverrideSet result;
	for (const auto &entry : setup.role_assignments) {
		if (const auto role = routing::ai_setup_role_from_string(entry.role)) {
			result.push_back({*role, {entry.provider_id, entry.model_id}});
		}
	}
	return result;
}

routing::AiSetupOverrideSet project_overrides(const matters::Matter &matter) {
	routing::AiSetupOverrideSet result;
	for (const auto &entry : matter.ai_setup_overrides) {
		if (const auto role = routing::ai_setup_role_from_string(entry.role)) {
			result.push_back({*role, {entry.provider_id, entry.model_id}});
		}
	}
	return result;
}

routing::ResolvedRoleAssignment require_context_assignment(
		const matters::Matter &matter, const platform::LocalNodeSetup &setup) {
	const auto resolved = routing::resolve_role(routing::AiSetupRole::Context, node_default_overrides(setup),
			project_overrides(matter), {}, {});
	if (!resolved.model) {
		throw std::runtime_error(
				"Automatic Context is paused because no Context model is assigned in AI Setup. You can continue editing Context manually.");
	}
	routing::validate_role_model_assignment(routing::AiSetupRole::Context, *resolved.model);
	return resolved;
}

bool automatic_update_available(const matters::Matter &matter, const platform::LocalNodeSetup &setup) {
	try {
		const auto assignment = require_context_assignment(matter, setup);
		routing::InferenceRequest request;
		request.model_target = assignment.model;
		routing::require_execution_ready(request);
		return true;
	} catch (const std::exception &) {
		return false;
	}
}

std::vector<std::string> automatic_source_ids(const matters::Matter &matter) {
	const bool real_project = matter.directory_path && !matter.directory_path->empty();
	std::vector<std::string> result;
	for (const auto &document : matter.documents) {
		if (document.extraction_status != matters::DocumentExtractionState::Ready) continue;
		if (real_project && document.source_mode != matters::DocumentSourceMode::Referenced) continue;
		result.push_back(document.id);
	}
	return result;
}

std::string normalized_identity_text(const std::string &value) {
	// The Entity Registry's own surface form (case in every script, quotation
	// marks and spacing), so a candidate value names an entity exactly when a
	// mention of it would.
	return privacy::surface_alias_key(value);
}

std::optional<std::string> resolve_candidate_entity(
		const matters::Matter &matter, const context::ContextCandidate &candidate) {
	// ContextItem.entityId says what the item itself represents; it is not a
	// bag of every entity merely mentioned inside a composite fact.  Require
	// the complete candidate value to identify one Registry name.  This keeps
	// a "Business Day" definition containing London, for example, from
	// incorrectly becoming the London entity while exact party/person/date/
	// amount values still resolve to the Project's stable identity.
	const std::string candidate_value = normalized_identity_text(candidate.value);
	std::set<std::string> matches;
	for (const privacy::ProjectEntity &entity : privacy::list_active_entities(matter)) {
		const bool occurs_in_source = std::any_of(entity.occurrences.begin(), entity.occurrences.end(),
				[&](const privacy::EntityOccurrence &occurrence) {
					return occurrence.document_id == candidate.source.document_id;
				});
		if (!occurs_in_source) continue;
		std::vector<std::string> names = entity.aliases;
		names.push_back(entity.canonical_value);
		for (const std::string &name : names) {
			const std::string normalized = normalized_identity_text(name);
			if (!normalized.empty() && candidate_value == normalized) {
				matches.insert(entity.id);
				break;
			}
		}
	}
	return matches.size() == 1 ? std::optional<std::string>(*matches.begin()) : std::nullopt;
}

// What a failed automatic Context call still owes the usage record: the model
// it was actually sent to, and whatever the provider billed for the attempt
// (a truncated or refused generation is charged like any other).
void record_failed_attempt(matters::MatterTask &task, const routing::ResolvedRoleAssignment &assignment,
		const std::exception &error) {
	if (assignment.model) {
		if (!task.provider_id) task.provider_id = assignment.model->provider_id;
		if (!task.model_id) task.model_id = assignment.model->model_id;
	}
	if (const auto *unusable = dynamic_cast<const providers::UnusableGeneration *>(&error))
		task.usage = unusable->usage;
	else if (const auto *research = dynamic_cast<const singularity::research::RequiredResearchFailure *>(&error))
		task.usage = research->usage;
}

// The query automatic Context reads the Project with. One constant so every
// batch of one run asks the same question of every document.
constexpr const char *kAutomaticContextQuery =
		"parties roles people jurisdiction transaction documents definitions dates amounts structure facts";

// This build declares the output ceiling its own contract needs; what a
// resolved window actually leaves for it is Singularity's shared policy, applied
// from the one place that owns it. Automatic Context carries no capacity rule
// of its own and never estimates a window itself - the figure comes from the
// same resolver every other request uses (local inference's granted/planned capacity for
// a local model, the provider-reported window for a remote one).
std::size_t context_batch_reserved_output_tokens(const std::optional<std::size_t> &window) {
	return research::reserved_output_tokens_for_window(window, context::kContextBuildMaxOutputTokens);
}

// How much source material one batch may carry: what this model's own window
// has left after the answer it must be able to write and the instruction every
// build actually carries - through the same shared capacity law
// (admissible_research_input_tokens_for_window) and the same estimator the
// execution gate applies, with the instruction MEASURED rather than assumed.
// With capacity genuinely unknown it stays unknown: no batch ceiling is
// invented here, and the historical window remains the bound exactly as
// everywhere else.
std::optional<std::size_t> context_batch_source_ceiling(const std::optional<std::size_t> &window) {
	const auto admissible = research::admissible_research_input_tokens_for_window(
		window, context_batch_reserved_output_tokens(window));
	if (!admissible) return std::nullopt;
	const std::size_t instruction = research::estimate_tokens(context::context_build_instruction());
	if (*admissible <= instruction) return std::size_t{0};
	return *admissible - instruction;
}

// A document's own name as this Project records it, for saying truthfully which
// document could not be read.
std::string document_name(const matters::Matter &matter, const std::string &document_id) {
	for (const matters::MatterDocument &document : matter.documents)
		if (document.id == document_id) return document.name;
	return document_id;
}

// Every AI-ready document of this Project, grouped into batches whose own
// source material fits the Context model's window. A document that cannot be
// read at all is recorded in `unavailable` and left out, so one unreadable file
// never costs the Project its whole Context build (CTX-2).
std::vector<std::vector<std::string>> context_source_batches(
		const std::filesystem::path &workspace_path, const matters::Matter &matter,
		const std::vector<std::string> &source_ids, const routing::ResolvedRoleAssignment &assignment,
		std::vector<matters::UnavailableSourceDocument> &unavailable) {
	const std::optional<std::size_t> window = assignment.model
		? routing::estimate_model_context_window_tokens(*assignment.model) : std::nullopt;
	const std::optional<std::size_t> batchTokenCeiling = context_batch_source_ceiling(window);
	const std::size_t instructionTokens = research::estimate_tokens(context::context_build_instruction());
	std::vector<std::vector<std::string>> batches;
	std::vector<std::string> current;
	std::size_t currentTokens = 0;
	for (const std::string &document_id : source_ids) {
		std::vector<matters::UnavailableSourceDocument> documentUnavailable;
		std::vector<context::RetrievedContextChunk> chunks;
		try {
			// Every passage of this one document, so what it would really
			// contribute to a batch is measured rather than guessed - the
			// historical eight-passage relevance window is what made automatic
			// Context read only the first few documents of a Project at all.
			chunks = matters::retrieve_document_context(workspace_path, matter, kAutomaticContextQuery,
				{document_id}, &documentUnavailable, matters::SelectedSourceDelivery::EveryPassage);
		} catch (const std::exception &) {
			// A document that cannot be read now is one document missing from
			// this build, not a reason to abandon the Project's whole Context.
			documentUnavailable.push_back(matters::UnavailableSourceDocument{document_id, document_name(matter,
				document_id), "unreadable"});
		}
		if (!documentUnavailable.empty()) {
			for (matters::UnavailableSourceDocument &entry : documentUnavailable)
				unavailable.push_back(std::move(entry));
			continue;
		}
		if (chunks.empty()) continue;
		// Measured as this document will REALLY be sent - the formatted source
		// material with its own markers and headings, not the raw text, which
		// undercounts what the prompt actually carries.
		const std::size_t documentTokens = research::estimate_tokens(
			context::prepare_context_build(chunks).prompt) - instructionTokens;
		// A single document larger than one whole batch still gets its own
		// batch: retrieval bounds what it actually contributes, and skipping it
		// would be a silent hole in the Project's own Context.
		if (!current.empty() && batchTokenCeiling && currentTokens + documentTokens > *batchTokenCeiling) {
			batches.push_back(std::move(current));
			current.clear();
			currentTokens = 0;
		}
		current.push_back(document_id);
		currentTokens += documentTokens;
	}
	if (!current.empty()) batches.push_back(std::move(current));
	return batches;
}

matters::MatterTask context_operation_base(const routing::ResolvedRoleAssignment &assignment,
		const std::vector<std::string> &source_ids, const std::string &timestamp) {
	matters::MatterTask task;
	task.id = matters::generate_task_id();
	task.created_at = timestamp;
	task.operation_type = "context_automation";
	task.operation_role = "context";
	task.ai_setup_source = routing::to_string(assignment.source);
	task.prompt = "Automatic Context update";
	task.legal_task_preset_id = "context-automation";
	task.prompt_profile_id = "context-automation";
	task.selected_document_ids = source_ids;
	if (assignment.model) {
		task.requested_provider_id = assignment.model->provider_id;
		task.requested_model_id = assignment.model->model_id;
	}
	return task;
}

void apply_inference_evidence(matters::MatterTask &task, const routing::InferenceResult &inference) {
	task.provider_id = inference.provider_id;
	task.model_id = inference.model_id;
	task.execution_kind = inference.execution_kind;
	task.runtime_id = inference.runtime_id;
	task.runtime_version = inference.runtime_version;
	task.runtime_source_commit = inference.runtime_source_commit;
	task.runtime_platform = inference.runtime_platform;
	task.runtime_backend = inference.runtime_backend;
	task.runtime_model_id = inference.runtime_model_id;
	task.protected_api_mode = inference.privacy_metadata.mode;
	task.protected_api_applied = inference.protected_api_applied;
	task.privacy_metadata = inference.privacy_metadata;
	// An automatic Context build pays for one call per bounded batch, so what it
	// cost is what ALL of them cost - accumulated through the one shared rule
	// (providers::accumulate_usage), which also keeps the total honest when a
	// batch came back with real usage but no price.
	task.usage = providers::accumulate_usage(task.usage, inference.usage);
}

std::vector<routing::ProjectEntityProtectionHint> entity_protection_hints(const matters::Matter &matter) {
	std::vector<routing::ProjectEntityProtectionHint> result;
	const auto symbols = privacy::project_entity_symbols(matter);
	for (const privacy::ProjectEntity &entity : privacy::list_active_entities(matter)) {
		routing::ProjectEntityProtectionHint hint;
		hint.entity_id = entity.id;
		hint.category = entity.category;
		hint.local_values = privacy::protection_values(matter, entity);
		hint.curated_values = entity.locked_aliases;
		if (const auto symbol = symbols.find(entity.id); symbol != symbols.end()) hint.symbol = symbol->second;
		result.push_back(std::move(hint));
	}
	return result;
}

} // namespace

std::optional<MemoryModel> memory_model(const std::string &id) {
	std::optional<routing::ModelTarget> assigned;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const matters::Matter matter = require_matter(id);
		const platform::AppSettings settings = platform::load_app_settings();
		const auto resolved = routing::resolve_role(routing::AiSetupRole::Context,
			node_default_overrides(settings.local_node_setup), project_overrides(matter), {}, {});
		assigned = resolved.model;
	}
	if (!assigned) return std::nullopt;
	return MemoryModel{*assigned, assigned->provider_id == "local" ? "local" : "external"};
}

std::vector<routing::ProjectEntityProtectionHint> memory_protection_hints(const std::string &id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	return entity_protection_hints(require_matter(id));
}

std::vector<ContextItemView> list_items(const std::string &id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const matters::Matter matter = require_matter(id);
	std::vector<ContextItemView> views;
	for (const context::ContextItem &item : context::list_items(matter))
		views.push_back({item, context::context_item_standing(matter, item)});
	return views;
}

std::vector<context::ContextSuggestion> list_pending_suggestions(const std::string &id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	return context::list_pending_suggestions(require_matter(id));
}

std::vector<context::ContextSearchResult> search_items(const std::string &id, const std::string &query) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	return context::search_items(require_matter(id), query);
}

context::ContextPageState get_page_state(const std::string &id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const matters::Matter matter = require_matter(id);
	return context::context_page_state(matter, count_context_conflicts(matter),
			automatic_update_available(matter, platform::load_app_settings().local_node_setup));
}

AutomaticContextUpdateResult run_automatic_update(const std::string &id) {
	const auto workspace_path = node::require_active_workspace_path();
	context::PreparedContextBuild prepared;
	routing::ResolvedRoleAssignment assignment;
	std::optional<std::string> workspace_id;
	std::vector<std::string> source_ids;
	std::vector<routing::ProjectEntityProtectionHint> project_entity_hints;
	// CTX-2: the batches this run reads the Project in. Automatic Context used
	// to retrieve one fixed window of eight passages, which on any real Project
	// meant it read the first few documents and silently called that the
	// Project's Context. Every AI-ready document is covered here instead, in
	// batches the Context model's own window can actually hold, and a document
	// that cannot be read is skipped and said out loud rather than ending the
	// whole run.
	std::vector<std::vector<std::string>> batches;
	std::vector<matters::UnavailableSourceDocument> unavailable;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const matters::Matter matter = require_matter(id);
		const platform::AppSettings settings = platform::load_app_settings();
		assignment = require_context_assignment(matter, settings.local_node_setup);
		workspace_id = settings.local_node_setup.active_workspace_id;
		source_ids = automatic_source_ids(matter);
		project_entity_hints = entity_protection_hints(matter);
		batches = context_source_batches(workspace_path, matter, source_ids, assignment, unavailable);
		if (batches.empty()) {
			// Exactly the historical refusal: nothing readable to build from.
			prepared = context::prepare_context_build({});
		}
	}

	matters::MatterTask operation = context_operation_base(
			assignment, source_ids, workspaces::current_timestamp());
	operation.unavailable_source_documents = unavailable;
	routing::InferenceResult inference;
	std::vector<context::ContextCandidate> candidates;
	try {
		// One bounded call per batch, each reading its own documents and
		// returning candidates for them. What every batch actually covered is
		// recorded, so "this is your Project's Context" is a statement about the
		// whole Project rather than about whatever fitted one window.
		matters::RequestCoverage coverage;
		std::set<std::string> admittedCandidates;
		for (const std::vector<std::string> &batch : batches) {
			context::PreparedContextBuild batchPrepared;
			{
				const persistence::PersistenceLock lock(persistence::persistence_mutex());
				const matters::Matter matter = require_matter(id);
				const auto chunks = matters::retrieve_document_context(
						workspace_path, matter, kAutomaticContextQuery, batch, nullptr,
						matters::SelectedSourceDelivery::EveryPassage);
				if (chunks.empty()) continue;
				batchPrepared = context::prepare_context_build(chunks);
			}
			routing::InferenceRequest request;
			request.call_purpose = routing::observability::CallPurpose::ContextAutomation;
			request.prompt = batchPrepared.prompt;
			request.workspace_id = workspace_id;
			request.model_target = assignment.model;
			// Sixteen concise, source-backed candidates can exceed the generic local
			// 512-token budget before the JSON object closes.  Give either shared
			// routing branch the same bounded operation budget - bounded in turn by
			// what this model's own window can actually spare, so a small local
			// model reserves an answer it has room to write instead of refusing the
			// whole request. The model still stops at its normal end-of-generation
			// signal when it finishes earlier.
			request.maximum_output_tokens = static_cast<int>(context_batch_reserved_output_tokens(
				assignment.model ? routing::estimate_model_context_window_tokens(*assignment.model)
					: std::nullopt));
			request.project_entity_hints = project_entity_hints;
			inference = routing::execute(request);
			apply_inference_evidence(operation, inference);
			operation.response_text = inference.text;
			for (context::ContextCandidate &candidate :
					context::parse_context_candidates(inference.text, batchPrepared.supplied_chunks)) {
				// One value per (section, label, value) across the whole run: two
				// batches naming the same fact state it once.
				const std::string key = std::string(context::to_string(candidate.section)) + "\n"
					+ candidate.label + "\n" + candidate.value;
				if (!admittedCandidates.insert(key).second) continue;
				candidates.push_back(std::move(candidate));
			}
			// Per-document coverage, from what this batch actually supplied.
			std::map<std::string, matters::SourceCoverage> byDocument;
			for (const context::RetrievedContextChunk &chunk : batchPrepared.supplied_chunks) {
				matters::SourceCoverage &entry = byDocument[chunk.document_id];
				entry.document_id = chunk.document_id;
				++entry.supplied;
				++entry.retrieved;
			}
			for (auto &entry : byDocument) coverage.sources.push_back(std::move(entry.second));
		}
		coverage.relevance_selection = true;
		coverage.relevance_candidate_documents = source_ids.size();
		operation.request_coverage = coverage;
		operation.status = matters::TaskStatus::Success;
		if (candidates.empty() && batches.empty())
			throw std::runtime_error("No processed Project documents are available yet. You can continue editing "
				"Context manually.");
	} catch (const providers::TaskError &error) {
		operation.status = matters::TaskStatus::Error;
		operation.error_code = error.code();
		operation.error = error.what();
		// A failed call the provider still billed keeps its usage and the model
		// it was sent to: automatic Context is real spend like any other.
		record_failed_attempt(operation, assignment, error);
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		auto found = matters::load_matter(workspace_path, id);
		if (found) { found->tasks.push_back(operation); matters::save_matter(workspace_path, *found); }
		throw;
	} catch (const std::exception &error) {
		operation.status = matters::TaskStatus::Error;
		operation.error_code = providers::TaskErrorCode::Unknown;
		operation.error = error.what();
		record_failed_attempt(operation, assignment, error);
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		auto found = matters::load_matter(workspace_path, id);
		if (found) { found->tasks.push_back(operation); matters::save_matter(workspace_path, *found); }
		throw;
	}

	context::ContextBuildApplication application;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		auto found = matters::load_matter(workspace_path, id);
		if (!found) throw std::runtime_error("This project no longer exists.");
		matters::Matter working_copy = *found;
		for (context::ContextCandidate &candidate : candidates) {
			candidate.entity_id = resolve_candidate_entity(working_copy, candidate);
		}
		const std::string timestamp = workspaces::current_timestamp();
		application = context::apply_context_candidates(working_copy, candidates, timestamp);
		operation.result_context_item_ids = application.item_ids;
		operation.result_context_suggestion_ids = application.suggestion_ids;
		std::set<std::string> seen_references;
		for (const context::ContextCandidate &candidate : candidates) {
			if (seen_references.insert(candidate.source.chunk_id).second)
				operation.source_references.push_back(candidate.source);
		}
		working_copy.tasks.push_back(operation);
		working_copy.updated_at = timestamp;
		*found = std::move(working_copy);
		matters::save_matter(workspace_path, *found);
	}
	return AutomaticContextUpdateResult{
			application, operation.id, inference.provider_id, inference.model_id, inference.execution_kind,
			routing::to_string(assignment.source), inference.runtime_id, inference.runtime_version,
			inference.runtime_source_commit, inference.runtime_platform, inference.runtime_backend,
			inference.runtime_model_id, inference.privacy_metadata, inference.protected_api_applied};
}

context::ContextItem accept_suggestion(const std::string &id, const std::string &suggestion_id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto found = matters::load_matter(workspace_path, id);
	if (!found) throw std::runtime_error("This project no longer exists.");
	const std::string timestamp = workspaces::current_timestamp();
	const std::string actor_id = platform::resolve_actor_identity().actor_id;
	const std::string item_id = context::accept_suggestion(*found, suggestion_id, actor_id, timestamp);
	found->updated_at = timestamp;
	matters::save_matter(workspace_path, *found);

	const auto item = context::find_item(*found, item_id);
	if (!item) throw std::runtime_error("This Context item no longer exists.");
	return *item;
}

void dismiss_suggestion(const std::string &id, const std::string &suggestion_id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto found = matters::load_matter(workspace_path, id);
	if (!found) throw std::runtime_error("This project no longer exists.");
	const std::string timestamp = workspaces::current_timestamp();
	const std::string actor_id = platform::resolve_actor_identity().actor_id;
	context::dismiss_suggestion(*found, suggestion_id, actor_id, timestamp);
	found->updated_at = timestamp;
	matters::save_matter(workspace_path, *found);
}

context::ContextItem create_item(
		const std::string &id, context::ContextSection section, const std::string &label, const std::string &value) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto found = matters::load_matter(workspace_path, id);
	if (!found) throw std::runtime_error("This project no longer exists.");
	const std::string timestamp = workspaces::current_timestamp();
	const std::string actor_id = platform::resolve_actor_identity().actor_id;
	const context::ContextItem item = context::create_item(*found, section, label, value, actor_id, timestamp);
	found->updated_at = timestamp;
	matters::save_matter(workspace_path, *found);
	return item;
}

context::ContextItem create_item_from_chat_result(const std::string &id, context::ContextSection section,
		const std::string &label, const std::string &value, const std::string &matter_task_id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto found = matters::load_matter(workspace_path, id);
	if (!found) throw std::runtime_error("This project no longer exists.");
	const std::string timestamp = workspaces::current_timestamp();
	const std::string actor_id = platform::resolve_actor_identity().actor_id;
	const context::ContextItem item =
			context::create_item_from_chat_result(*found, section, label, value, matter_task_id, actor_id, timestamp);
	found->updated_at = timestamp;
	matters::save_matter(workspace_path, *found);
	return item;
}

context::ContextItem update_item_value(const std::string &id, const std::string &item_id,
		const std::string &value, const std::optional<context::ContextItem> &base_item) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto found = matters::load_matter(workspace_path, id);
	if (!found) throw std::runtime_error("This project no longer exists.");
	bool stale_edit = false;
	if (base_item) {
		if (base_item->id != item_id) throw std::runtime_error("The Context edit baseline does not match this item.");
		const auto current = context::find_item(*found, item_id);
		if (!current) throw std::runtime_error("This Context item no longer exists.");
		const nlohmann::json current_json = matters::serialize_context_item_storage(*current);
		const nlohmann::json base_json = matters::serialize_context_item_storage(*base_item);
		stale_edit = current_json != base_json;
		if (stale_edit) {
			// Represent the caller's independently loaded baseline as the base of
			// a normal ProjectStore operation.  ProjectStore's ordinary replay
			// then observes current != base != attempted and creates the durable
			// conflict; this code never writes a conflict record itself.
			nlohmann::json baseline = matters::serialize_matter_storage(*found);
			for (nlohmann::json &entry : baseline["contextItems"]) {
				if (entry.value("id", std::string()) == item_id) {
					entry = base_json;
					break;
				}
			}
			for (context::ContextItem &item : found->context_items) {
				if (item.id == item_id) {
					item = *base_item;
					break;
				}
			}
			if (!found->storage_baseline) throw std::runtime_error("Open the Project again before editing Context.");
			auto edit_baseline = std::make_shared<matters::ProjectStorageBaseline>(*found->storage_baseline);
			edit_baseline->snapshot = baseline.dump();
			found->storage_baseline = std::move(edit_baseline);
		}
	}
	const std::string timestamp = workspaces::current_timestamp();
	const std::string actor_id = platform::resolve_actor_identity().actor_id;
	const context::ContextItem item = context::update_item_value(*found, item_id, value, actor_id, timestamp);
	found->updated_at = timestamp;
	matters::save_matter(workspace_path, *found);
	if (stale_edit) {
		throw std::runtime_error(
				"This Context item changed in another session. The competing edit was preserved for review.");
	}
	return item;
}

void delete_item(const std::string &id, const std::string &item_id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto found = matters::load_matter(workspace_path, id);
	if (!found) throw std::runtime_error("This project no longer exists.");
	const std::string timestamp = workspaces::current_timestamp();
	context::delete_item(*found, item_id, platform::resolve_actor_identity().actor_id, timestamp);
	found->updated_at = timestamp;
	matters::save_matter(workspace_path, *found);
}

context::ContextItem link_item_entity(const std::string &id, const std::string &item_id, const std::string &entity_id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto found = matters::load_matter(workspace_path, id);
	if (!found) throw std::runtime_error("This project no longer exists.");
	const std::string timestamp = workspaces::current_timestamp();
	const std::string actor_id = platform::resolve_actor_identity().actor_id;
	const context::ContextItem item = context::link_item_entity(*found, item_id, entity_id, actor_id, timestamp);
	found->updated_at = timestamp;
	matters::save_matter(workspace_path, *found);
	return item;
}

context::ContextItem unlink_item_entity(const std::string &id, const std::string &item_id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto found = matters::load_matter(workspace_path, id);
	if (!found) throw std::runtime_error("This project no longer exists.");
	const std::string timestamp = workspaces::current_timestamp();
	const std::string actor_id = platform::resolve_actor_identity().actor_id;
	const context::ContextItem item = context::unlink_item_entity(*found, item_id, actor_id, timestamp);
	found->updated_at = timestamp;
	matters::save_matter(workspace_path, *found);
	return item;
}

context::ContextItem update_item_controls(const std::string &id, const std::string &item_id,
		context::ContextItemKind kind, bool is_pinned, bool is_locked, bool is_excluded_from_ai) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto found = matters::load_matter(workspace_path, id);
	if (!found) throw std::runtime_error("This project no longer exists.");
	const std::string timestamp = workspaces::current_timestamp();
	const std::string actor_id = platform::resolve_actor_identity().actor_id;
	const context::ContextItem item = context::update_item_controls(
			*found, item_id, kind, is_pinned, is_locked, is_excluded_from_ai, actor_id, timestamp);
	found->updated_at = timestamp;
	matters::save_matter(workspace_path, *found);
	return item;
}

std::vector<context::ContextMergeConflict> list_merge_conflicts(const std::string &id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const matters::Matter matter = require_matter(id);
	if (matter.directory_path.has_value() == false || persistence::has_project_data(*matter.directory_path) == false) {
		return {};
	}
	const persistence::ProjectRecord record = persistence::load_project(*matter.directory_path);
	std::vector<context::ContextMergeConflict> result;
	for (const persistence::ProjectConflict &conflict : record.conflicts) {
		if (conflict.collection != "contextItems") continue;
		const nlohmann::json current = nlohmann::json::parse(conflict.current_json);
		const nlohmann::json attempted = nlohmann::json::parse(conflict.attempted_json);
		context::ContextMergeConflict item;
		item.id = conflict.id;
		item.item_id = conflict.object_id;
		item.label = current.is_object()
				? current.value("label", std::string("Context item"))
				: attempted.is_object() ? attempted.value("label", std::string("Context item")) : "Context item";
		item.current_value = current.is_object() ? current.value("value", std::string()) : std::string();
		if (attempted.is_object()) item.authored_value = attempted.value("value", std::string());
		item.current_actor_id = conflict.current_actor_id;
		item.authored_actor_id = conflict.actor_id;
		item.current_timestamp = conflict.current_timestamp;
		item.authored_timestamp = conflict.timestamp;
		result.push_back(std::move(item));
	}
	return result;
}

std::optional<context::ContextItem> resolve_merge_conflict(const std::string &id, const std::string &conflict_id,
		const std::string &resolution, const std::optional<std::string> &edited_value) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const matters::Matter matter = require_matter(id);
	if (matter.directory_path.has_value() == false || persistence::has_project_data(*matter.directory_path) == false) {
		throw std::runtime_error("This Project does not have shared Project data.");
	}
	persistence::ProjectRecord record = persistence::load_project(*matter.directory_path);
	const auto found = std::find_if(record.conflicts.begin(), record.conflicts.end(), [&](const persistence::ProjectConflict &conflict) {
		return conflict.id == conflict_id && conflict.collection == "contextItems";
	});
	if (found == record.conflicts.end()) throw std::runtime_error("This Context conflict was already resolved.");

	const std::string timestamp = workspaces::current_timestamp();
	const platform::ActorIdentity actor = platform::resolve_actor_identity();
	if (resolution == "use_authored") {
		const nlohmann::json attempted = nlohmann::json::parse(found->attempted_json);
		if (attempted.is_null()) {
			context::delete_item(record.project, found->object_id, actor.actor_id, timestamp);
		} else {
			const context::ContextItem authored_item = matters::deserialize_context_item_storage(attempted);
			if (authored_item.id != found->object_id) {
				throw std::runtime_error("The concurrent Context item does not match this conflict.");
			}
			context::replace_item_from_conflict(record.project, authored_item, actor.actor_id, timestamp);
		}
	} else if (resolution == "edit") {
		if (edited_value.has_value() == false || edited_value->empty() == true) {
			throw std::runtime_error("Enter the value that should become current.");
		}
		if (context::find_item(record.project, found->object_id)) {
			context::update_item_value(record.project, found->object_id, *edited_value, actor.actor_id, timestamp);
		} else {
			const nlohmann::json attempted = nlohmann::json::parse(found->attempted_json);
			if (attempted.is_object() == false) {
				throw std::runtime_error("There is no Context item to restore with this value.");
			}
			context::ContextItem combined_item = matters::deserialize_context_item_storage(attempted);
			combined_item.value = *edited_value;
			context::replace_item_from_conflict(record.project, combined_item, actor.actor_id, timestamp);
		}
	} else if (resolution != "keep_current") {
		throw std::runtime_error("Choose how to resolve this Context conflict.");
	}

	record.project.updated_at = timestamp;
	persistence::save_project_conflict_resolution(record.project, actor, conflict_id);
	return context::find_item(record.project, found->object_id);
}

} // namespace singularity::context_actions
