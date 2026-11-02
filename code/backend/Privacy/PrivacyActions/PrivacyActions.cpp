#include "Privacy/PrivacyActions/PrivacyActions.hpp"

#include "Document/DocumentCache/DocumentCache.hpp"
#include "Document/DocumentChunker/DocumentChunker.hpp"
#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "PapinianGuard/Policy/Policy.hpp"
#include "PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "PapinianGuard/PapinianGuardService/PapinianGuardService.hpp"
#include "PapinianGuard/Taxonomy/Taxonomy.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Privacy/EntityCuration/EntityCuration.hpp"
#include "Privacy/ProjectPreprocessing/ProjectPreprocessing.hpp"
#include "Routing/InferenceExecutor/InferenceExecutor.hpp"
#include "Routing/ProtectedInference/SourceMaterialSpans.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace singularity::privacy_actions {
namespace {

// One Project, read on its own: protection work and the Guard page concern
// this Project only, so they never pay for - or wait on - the rest of the
// workspace (matters::load_matter).
matters::Matter require_matter(const std::string &id) {
	auto found = matters::load_matter(node::require_active_workspace_path(), id);
	if (!found) throw std::runtime_error("This project no longer exists.");
	return std::move(*found);
}

// Every internal preprocessing/readiness check uses Standard regardless of
// the user's own outbound Guard policy: Entity Registry maintenance is
// purely local (PRIVACY_CONTRACT.md - "Preprocessing never means the
// document was sent outside"), so an explicit outbound "Off" must not also
// disable this local-only feature. This mirrors InferenceExecutor.cpp's own
// warm-up policy for the identical reason.
protected_api::Policy internal_detection_policy() {
	protected_api::Policy policy;
	policy.mode = protected_api::Mode::Standard;
	return policy;
}

// Points the production guard's durable detection memory at this workspace.
void configure_semantic_detection_store(const std::filesystem::path &workspace_path,
		const protected_api::papinian::SemanticModelIdentity &model) {
	using protected_api::papinian::SemanticDetectionStore;
	const std::filesystem::path directory = workspace_path / ".singularity-workspace" / "derivatives" / "papinian-detections";
	const auto current = protected_api::papinian::process_semantic_detection_store();
	if (current && current->directory() == directory && current->serves(model)) return;
	const auto store = std::make_shared<SemanticDetectionStore>(directory, model);
	// Once per process: entries an earlier format wrote can never be read again.
	(void)store->prune_foreign_entries();
	protected_api::papinian::set_process_semantic_detection_store(store);
}

// Set once the app is closing: long local passes stop between documents
// instead of holding the process open.
std::atomic<bool> &stop_requested() {
	static std::atomic<bool> flag{false};
	return flag;
}

// The title a prompt shows above the document's passages, as a send presents
// it to detection (SourceMaterialSpans' document_header()).
std::string title_passage(const matters::MatterDocument &document) {
	return routing::source_material::as_prompt_text(
		routing::source_material::document_header_title(matters::placed_name(document)));
}

// What one warm-up of a document covers: its passages as read, and its title
// where it stands now - a document moved to another folder is warmed again.
std::string detection_unit(const matters::MatterDocument &document) {
	return document.fingerprint + "\n" + document.extractor_version + "\nchunks-default\n" + title_passage(document);
}

// A document's reading on this computer: the one the Matter's record names,
// or this computer's own. A Matter shared with a computer of another system
// names that computer's reader (a PDF read on Windows), while this one keeps
// its own reading of the same file; without it, nothing here was prepared for
// such a document (2026-11-02).
std::optional<documents::DocumentExtractionResult> load_reading(const documents::DocumentCache &cache,
		const matters::MatterDocument &document) {
	if (auto extraction = cache.load(document.fingerprint, document.extractor_version)) return extraction;
	const std::string own = documents::local_extractor_version(document.file_type);
	if (own.empty() || own == document.extractor_version) return std::nullopt;
	return cache.load(document.fingerprint, own);
}

// One document's passages exactly as a send presents them to detection, or
// nothing when its extraction is not cached yet.
std::optional<std::vector<std::string>> load_passages(const documents::DocumentCache &cache,
		const matters::MatterDocument &document) {
	const auto extraction = load_reading(cache, document);
	if (!extraction) return std::nullopt;
	std::vector<std::string> passages;
	for (const auto &chunk : documents::chunk_document(document.id, document.fingerprint, *extraction)) {
		passages.push_back(routing::source_material::as_prompt_text(chunk.text));
	}
	return passages;
}

// Each document's masked reading, kept while the app is open and given back
// at once while nothing it was made from has changed: the file and its
// reading, its place, the Project's identities and this computer's Guard
// settings (the owner, 2026-11-02: "Papinian must read it ahead, and it must
// be cached and reused if the file did not change"). In memory only, never
// written: like a request's table, what it holds is never kept (AG-SEC-3).
struct PreviewCache {
	std::mutex mutex;
	std::list<std::pair<std::string, DocumentProtectionPreview>> entries;
};

PreviewCache &preview_cache() {
	static PreviewCache cache;
	return cache;
}

constexpr std::size_t kPreviewsKept = 96;

std::optional<DocumentProtectionPreview> kept_preview(const std::string &key) {
	PreviewCache &cache = preview_cache();
	const std::lock_guard<std::mutex> guard(cache.mutex);
	const auto found = std::find_if(cache.entries.begin(), cache.entries.end(),
			[&](const auto &entry) { return entry.first == key; });
	if (found == cache.entries.end()) return std::nullopt;
	cache.entries.splice(cache.entries.begin(), cache.entries, found);
	return cache.entries.front().second;
}

void keep_preview(const std::string &key, const DocumentProtectionPreview &preview) {
	PreviewCache &cache = preview_cache();
	const std::lock_guard<std::mutex> guard(cache.mutex);
	cache.entries.remove_if([&](const auto &entry) { return entry.first == key; });
	cache.entries.emplace_front(key, preview);
	while (cache.entries.size() > kPreviewsKept) cache.entries.pop_back();
}

// What a policy masks, as one line: its mode, categories and terms.
std::string policy_signature(const protected_api::Policy &policy) {
	std::string signature = std::to_string(static_cast<int>(policy.mode));
	for (const auto category : policy.custom_categories) signature += "," + std::to_string(static_cast<int>(category));
	for (const std::string &term : policy.custom_terms) signature += "\x1f" + term;
	return signature;
}

// Scores every passage of every protected document of one Project with the
// local model once, so sends reuse the result (SemanticDetectionStore). A
// document protected by run_preprocessing() is already scored this way and
// is skipped. Runs without the persistence lock and never throws.
void warm_project_detections(const std::string &project_id) {
	try {
		if (routing::is_papinian_model_loading()) return;
		const auto &guard = routing::production_guard();
		const auto store = protected_api::papinian::process_semantic_detection_store();
		if (!store) return;
		const auto workspace_path = node::require_active_workspace_path();
		matters::Matter project;
		{
			const persistence::PersistenceLock lock(persistence::persistence_mutex());
			project = require_matter(project_id);
		}
		const documents::LocalDocumentExtractor extractor;
		const documents::DocumentCache cache(workspace_path, extractor);
		for (const matters::MatterDocument &document : project.documents) {
			if (stop_requested().load()) return;
			const auto protection = std::find_if(project.document_protection.begin(), project.document_protection.end(),
					[&](const privacy::DocumentProtectionStatus &entry) { return entry.document_id == document.id; });
			if (protection == project.document_protection.end()
					|| protection->state != privacy::DocumentProtectionState::Ready) continue;
			const std::string unit = detection_unit(document);
			if (!store->is_marked(unit)) {
				auto passages = load_passages(cache, document);
				if (!passages) continue;
				passages->push_back(title_passage(document));
				(void)guard.warm_semantic_detections(*passages);
				store->mark(unit);
			}
			// Then masked once, so the document opens read at once (D-233).
			try {
				(void)preview_document_protection(project_id, document.id);
			} catch (const std::exception &) {
			}
		}
	} catch (const std::exception &error) {
		std::cerr << "Papinian Guard: document warm-up skipped: " << error.what() << '\n';
	} catch (...) {
	}
}

// At most one protection pass per Project at a time. A second caller gets
// the Project as it stands instead of scoring the same documents again.
class ProjectPassClaim final {
public:
	explicit ProjectPassClaim(std::string project_id) : project_id_(std::move(project_id)) {
		const std::lock_guard<std::mutex> lock(mutex());
		acquired_ = running().insert(project_id_).second;
	}
	~ProjectPassClaim() {
		if (acquired_ == false) return;
		const std::lock_guard<std::mutex> lock(mutex());
		running().erase(project_id_);
	}
	ProjectPassClaim(const ProjectPassClaim &) = delete;
	ProjectPassClaim &operator=(const ProjectPassClaim &) = delete;
	bool acquired() const { return acquired_; }
	static bool held(const std::string &project_id) {
		const std::lock_guard<std::mutex> lock(mutex());
		return running().count(project_id) > 0;
	}

private:
	static std::mutex &mutex() {
		static std::mutex value;
		return value;
	}
	static std::set<std::string> &running() {
		static std::set<std::string> value;
		return value;
	}
	std::string project_id_;
	bool acquired_ = false;
};

enum class ProtectionJob { Preprocess, Warm };

// One background thread does all local protection work, in order: a
// Documents read or launch never waits for model scoring, and two passes
// never compete for the same cores. Never destroyed: the app stops it
// explicitly (stop_background_protection) before static teardown.
struct ProtectionWorker {
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<std::pair<ProtectionJob, std::string>> pending;
	std::thread thread;
	bool stopping = false;
};

ProtectionWorker &protection_worker() {
	static ProtectionWorker *worker = new ProtectionWorker();
	return *worker;
}

void run_protection_jobs() {
	auto &worker = protection_worker();
	for (;;) {
		std::pair<ProtectionJob, std::string> job;
		{
			std::unique_lock<std::mutex> lock(worker.mutex);
			worker.wake.wait(lock, [&] { return worker.stopping || !worker.pending.empty(); });
			if (worker.stopping) return;
			job = worker.pending.front();
			worker.pending.pop_front();
		}
		try {
			if (job.first == ProtectionJob::Preprocess) (void)run_preprocessing(job.second);
			else warm_project_detections(job.second);
		} catch (const std::exception &error) {
			std::cerr << "Papinian Guard: document protection skipped: " << error.what() << '\n';
		} catch (...) {
		}
	}
}

void enqueue_protection_job(ProtectionJob kind, const std::string &project_id, bool first = false) {
	auto &worker = protection_worker();
	const std::lock_guard<std::mutex> lock(worker.mutex);
	if (worker.stopping) return;
	const auto job = std::make_pair(kind, project_id);
	const auto queued = std::find(worker.pending.begin(), worker.pending.end(), job);
	if (first) {
		if (queued != worker.pending.end()) worker.pending.erase(queued);
		worker.pending.push_front(job);
	} else if (queued == worker.pending.end()) {
		worker.pending.push_back(job);
	}
	if (worker.thread.joinable() == false) worker.thread = std::thread(run_protection_jobs);
	worker.wake.notify_one();
}

void schedule_detection_warmup(const std::string &project_id) {
	enqueue_protection_job(ProtectionJob::Warm, project_id);
}

} // namespace

bool preprocessing_running(const std::string &id) { return ProjectPassClaim::held(id); }

void request_preprocessing(const std::string &id, bool in_use) {
	enqueue_protection_job(ProtectionJob::Preprocess, id, in_use);
}


void ensure_semantic_detection_store() {
	try {
		if (routing::is_papinian_model_loading()) return;
		const auto &guard = routing::production_guard();
		const auto identity = guard.semantic_model_identity();
		if (!identity) return;
		configure_semantic_detection_store(node::require_active_workspace_path(), *identity);
	} catch (const std::exception &error) {
		std::cerr << "Papinian Guard: detection store not attached: " << error.what() << '\n';
	} catch (...) {
	}
}

void stop_background_protection() noexcept {
	stop_requested().store(true);
	auto &worker = protection_worker();
	std::thread finishing;
	{
		const std::lock_guard<std::mutex> lock(worker.mutex);
		worker.stopping = true;
		worker.pending.clear();
		finishing = std::move(worker.thread);
	}
	worker.wake.notify_all();
	if (finishing.joinable()) finishing.join();
}

matters::Matter run_preprocessing(const std::string &id) {
	// While the model is still loading there is nothing to run and nothing to
	// record: the Project is returned as it is, and the next call after the
	// warm-up (Documents re-reads on its own interval) runs the pass. Joining
	// the warm-up here held a bridge worker for the whole load, and with a
	// few such reads in flight at Project open, even a plain Chat list queued
	// behind them for the load's duration.
	if (routing::is_papinian_model_loading()) {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		return require_matter(id);
	}
	try {
		routing::require_papinian_model_ready(internal_detection_policy());
	} catch (const std::exception &error) {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const auto workspace_path = node::require_active_workspace_path();
		auto found = matters::load_matter(workspace_path, id);
		if (!found) throw std::runtime_error("This project no longer exists.");
		const std::string timestamp = workspaces::current_timestamp();
		const std::string detail = std::string("Protection unavailable: ") + error.what();
		bool changed = false;
		for (const matters::MatterDocument &document : found->documents) {
			if (matters::document_lifecycle_state(document) != matters::DocumentLifecycleState::Ready) continue;
			const auto current = privacy::find_document_protection(*found, document.id);
			if (current && privacy::is_protection_current(*current, document)
					&& current->state == privacy::DocumentProtectionState::Ready) continue;
			// Already said, for this version of the document, for this same reason:
			// said again it would be a write each time the window reads the
			// documents, which asks for this pass again - a loop of one operation
			// a second on a computer without the model (2026-10-25).
			if (current && privacy::is_protection_current(*current, document)
					&& current->state == privacy::DocumentProtectionState::Unsupported && current->detail == detail) continue;
			privacy::DocumentProtectionStatus unavailable;
			unavailable.document_id = document.id;
			unavailable.state = privacy::DocumentProtectionState::Unsupported;
			unavailable.processed_fingerprint = document.fingerprint;
			unavailable.processed_extractor_version = document.extractor_version;
			unavailable.ner_model_version = "unavailable";
			unavailable.detail = detail;
			unavailable.updated_at = timestamp;
			const auto slot = std::find_if(found->document_protection.begin(), found->document_protection.end(),
					[&](const privacy::DocumentProtectionStatus &entry) { return entry.document_id == document.id; });
			if (slot == found->document_protection.end()) found->document_protection.push_back(unavailable);
			else *slot = unavailable;
			changed = true;
		}
		if (changed) { found->updated_at = timestamp; matters::save_matter(workspace_path, *found); }
		return *found;
	}

	const auto &guard = routing::production_guard();
	const auto identity = guard.semantic_model_identity();
	const std::string ner_model_version =
			identity ? (identity->checkpoint + "@" + identity->revision) : std::string("unknown");

	const auto workspace_path = node::require_active_workspace_path();
	if (identity) {
		configure_semantic_detection_store(workspace_path, *identity);
	}
	const auto store = identity ? protected_api::papinian::process_semantic_detection_store() : nullptr;

	const ProjectPassClaim claim(id);
	if (claim.acquired() == false) {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		return require_matter(id);
	}

	std::vector<matters::MatterDocument> pending;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const matters::Matter project = require_matter(id);
		for (const matters::MatterDocument &document : project.documents) {
			if (privacy::needs_protection_pass(project, document)) pending.push_back(document);
		}
	}

	// The model scores each document with no lock held - a data room takes
	// minutes, and every other read in the app shares this lock. The results
	// are then saved a few at a time, each against the Project as it stands
	// at that moment, so coverage grows as the pass goes and nothing a person
	// changed meanwhile is overwritten. A save writes the whole Project under
	// that lock: one save per document kept a large shared Matter's window
	// waiting for minutes (found on the Mac, 2026-10-25, as a received Matter
	// was protected document by document).
	struct Scored {
		matters::MatterDocument document;
		std::optional<privacy::PassageEntities> detections;
		std::string failure;
	};
	constexpr std::size_t kDocumentsPerSave = 8;
	constexpr std::chrono::seconds kSaveEvery{5};
	std::vector<Scored> scored;
	auto savedAt = std::chrono::steady_clock::now();
	const auto saveScored = [&]() {
		if (scored.empty()) return;
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		auto found = matters::load_matter(workspace_path, id);
		if (!found) throw std::runtime_error("This project no longer exists.");
		const std::string timestamp = workspaces::current_timestamp();
		std::vector<std::string> warmed;
		bool isChanged = false;
		for (const Scored &one : scored) {
			const auto current = std::find_if(found->documents.begin(), found->documents.end(),
					[&](const matters::MatterDocument &entry) { return entry.id == one.document.id; });
			// Changed, removed or already protected while it was being scored: the
			// next pass reads it again.
			if (current == found->documents.end() || current->fingerprint != one.document.fingerprint
					|| current->extractor_version != one.document.extractor_version
					|| privacy::needs_protection_pass(*found, *current) == false) continue;
			if (one.detections) {
				privacy::record_document_detection(*found, *current, *one.detections, ner_model_version, timestamp);
				if (store) warmed.push_back(detection_unit(*current));
			} else {
				privacy::record_document_detection_failure(*found, *current, ner_model_version, timestamp, one.failure);
			}
			isChanged = true;
		}
		if (isChanged) {
			found->updated_at = timestamp;
			matters::save_matter(workspace_path, *found);
		}
		for (const std::string &unit : warmed) store->mark(unit);
		scored.clear();
		savedAt = std::chrono::steady_clock::now();
	};
	const documents::LocalDocumentExtractor extractor;
	const documents::DocumentCache cache(workspace_path, extractor);
	for (const matters::MatterDocument &document : pending) {
		if (stop_requested().load()) break;
		const auto passages = load_passages(cache, document);
		if (!passages) continue;
		Scored one{document, std::nullopt, std::string()};
		try {
			one.detections = privacy::detect_passage_entities(*passages, guard);
			// The title stands above the passages in every prompt: prepared with
			// them, never counted among what the document itself holds.
			(void)guard.warm_semantic_detections({title_passage(document)});
		} catch (const std::exception &error) {
			one.failure = error.what();
		}
		scored.push_back(std::move(one));
		if (scored.size() >= kDocumentsPerSave || std::chrono::steady_clock::now() - savedAt >= kSaveEvery) saveScored();
	}
	saveScored();
	// Queued, never run here: the worker reads the Project again once this
	// pass has finished, so it sees every document just marked ready.
	if (identity) schedule_detection_warmup(id);
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	return require_matter(id);
}

DocumentProtectionPreview preview_document_protection(const std::string &id, const std::string &document_id) {
	DocumentProtectionPreview preview;
	protected_api::Policy policy;
	protected_api::papinian::OperationProtectedSymbolTable symbols;
	matters::MatterDocument document;
	std::filesystem::path workspace_path;
	std::string identities;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		policy = platform::load_app_settings().local_node_setup.protected_api_policy;
		workspace_path = node::require_active_workspace_path();
		const matters::Matter matter = require_matter(id);
		const auto found = std::find_if(matter.documents.begin(), matter.documents.end(),
				[&](const matters::MatterDocument &entry) { return entry.id == document_id; });
		if (found == matter.documents.end()) throw std::runtime_error("This document is no longer in the Matter.");
		document = *found;
		// The same Project identities a real request protects with, each under
		// its own symbol (D-235).
		symbols.project_entity_symbols = privacy::project_entity_symbols(matter);
		for (const privacy::ProjectEntity &entity : privacy::list_active_entities(matter)) {
			const std::vector<std::string> values = privacy::protection_values(matter, entity);
			symbols.project_entity_aliases[entity.id] = values;
			symbols.project_entity_types[entity.id] = entity.category;
			identities += entity.id + "\x1e" + protected_api::papinian::to_string(entity.category) + "\x1e"
					+ symbols.project_entity_symbols[entity.id];
			for (const std::string &value : values) identities += "\x1f" + value;
			for (const std::string &alias : entity.locked_aliases) identities += "\x1d" + alias;
			if (!entity.locked_aliases.empty())
				symbols.project_entity_curated_aliases[entity.id] =
						std::set<std::string>(entity.locked_aliases.begin(), entity.locked_aliases.end());
		}
	}
	preview.name = matters::placed_name(document);
	preview.protected_name = preview.name;
	preview.is_protected = policy.mode != protected_api::Mode::Off;
	const documents::LocalDocumentExtractor extractor;
	const documents::DocumentCache cache(workspace_path, extractor);
	const auto extraction = load_reading(cache, document);
	if (!extraction) return preview;
	const std::string key = id + "\n" + document.id + "\n" + document.fingerprint + "\n" + extraction->extractor_version
			+ "\n" + preview.name + "\n" + policy_signature(policy) + "\n" + std::to_string(std::hash<std::string>{}(identities));
	if (auto kept = kept_preview(key)) return std::move(*kept);
	preview.is_read = true;
	const auto chunks = documents::chunk_document(document.id, document.fingerprint, *extraction);
	preview.passage_count = chunks.size();
	if (preview.is_protected) routing::require_papinian_model_ready(policy);

	// Each symbol once, with every spelling it stood for in any passage.
	std::vector<DocumentProtectionPreview::Replacement> replacements;
	const auto keep = [&](const std::vector<protected_api::papinian::RequestMappingEntry> &mapping) {
		for (const auto &entry : mapping) {
			auto known = std::find_if(replacements.begin(), replacements.end(),
					[&](const DocumentProtectionPreview::Replacement &kept) { return kept.placeholder == entry.placeholder; });
			if (known == replacements.end()) {
				replacements.push_back({entry.placeholder,
						entry.type ? protected_api::papinian::to_string(*entry.type) : std::string("other"), {}});
				known = std::prev(replacements.end());
			}
			for (const std::string &form : entry.every_form())
				if (!form.empty() && std::find(known->forms.begin(), known->forms.end(), form) == known->forms.end())
					known->forms.push_back(form);
		}
	};
	const std::string request_namespace = routing::generate_request_namespace();
	const auto &guard = routing::production_guard();
	if (preview.is_protected) {
		const auto title = guard.protect(title_passage(document), policy, request_namespace, &symbols);
		preview.protected_name = title.protected_text;
		keep(title.mapping);
	}
	// A long document shows its beginning: what a person reads through here.
	constexpr std::size_t kMostBytes = 400 * 1024;
	std::size_t bytes = 0;
	for (const documents::DocumentChunk &chunk : chunks) {
		if (bytes >= kMostBytes) break;
		bytes += chunk.text.size();
		DocumentProtectionPreview::Passage passage;
		passage.text = chunk.text;
		passage.page = chunk.page;
		passage.heading = chunk.heading;
		if (preview.is_protected) {
			try {
				const auto protection = guard.protect(routing::source_material::as_prompt_text(chunk.text), policy,
						request_namespace, &symbols);
				passage.protected_text = protection.protected_text;
				keep(protection.mapping);
			} catch (const std::exception &) {
				passage.is_withheld = true;
			}
		} else {
			passage.protected_text = chunk.text;
		}
		preview.passages.push_back(std::move(passage));
	}
	preview.replacements = std::move(replacements);
	keep_preview(key, preview);
	return preview;
}

ProjectOverview get_overview(const std::string &id) {
	ProjectOverview overview;
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const matters::Matter matter = require_matter(id);
	overview.coverage = privacy::compute_coverage(matter);
	overview.attention_document_count =
			static_cast<long long>(privacy::list_attention_documents(matter).size());
	overview.entity_count = static_cast<long long>(privacy::list_active_entities(matter).size());
	overview.pending_ambiguity_count = privacy::count_pending_ambiguities(matter);
	if (routing::is_papinian_model_loading()) {
		overview.guard_status = "loading";
		overview.guard_message = "Papinian Guard is loading.";
		return overview;
	}
	try {
		routing::require_papinian_model_ready(internal_detection_policy());
		overview.guard_status = "ready";
	} catch (const std::exception &error) {
		overview.guard_status = "not_ready";
		overview.guard_message = error.what();
	}
	return overview;
}

std::vector<privacy::AttentionDocument> list_attention_documents(const std::string &id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	return privacy::list_attention_documents(require_matter(id));
}

std::vector<EntitySummary> list_entities(const std::string &id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const matters::Matter matter = require_matter(id);
	std::vector<EntitySummary> summaries;
	for (const auto &entity : privacy::list_active_entities(matter)) {
		EntitySummary summary;
		summary.entity = entity;
		summary.needs_review = privacy::pending_ambiguities_for(matter, entity.id).empty() == false;
		summaries.push_back(std::move(summary));
	}
	return summaries;
}

EntityDetail get_entity_detail(const std::string &id, const std::string &entity_id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const matters::Matter matter = require_matter(id);
	const auto entity = privacy::find_entity(matter, entity_id);
	if (!entity || entity->merged_into_entity_id) throw std::runtime_error("This entity no longer exists.");
	EntityDetail detail;
	detail.entity = *entity;
	detail.pending_ambiguities = privacy::pending_ambiguities_for(matter, entity_id);
	return detail;
}

std::string resolve_ambiguity(const std::string &id, const std::string &ambiguity_id, const std::string &decision) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto found = matters::load_matter(workspace_path, id);
	if (!found) throw std::runtime_error("This project no longer exists.");
	const std::string timestamp = workspaces::current_timestamp();

	std::string survivor_id;
	if (decision == "same") {
		survivor_id = privacy::resolve_ambiguity_same(*found, ambiguity_id, timestamp);
	} else if (decision == "separate") {
		privacy::resolve_ambiguity_separate(*found, ambiguity_id, timestamp);
	} else {
		throw std::runtime_error("This is not a real review decision.");
	}
	found->updated_at = timestamp;
	matters::save_matter(workspace_path, *found);
	return survivor_id;
}

std::string split_entity_alias(const std::string &id, const std::string &entity_id, const std::string &alias) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto found = matters::load_matter(workspace_path, id);
	if (!found) throw std::runtime_error("This project no longer exists.");
	const std::string timestamp = workspaces::current_timestamp();
	const std::string new_entity_id = privacy::split_entity_alias(*found, entity_id, alias, timestamp);
	found->updated_at = timestamp;
	matters::save_matter(workspace_path, *found);
	return new_entity_id;
}

void add_entity_alias(const std::string &id, const std::string &entity_id, const std::string &alias) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto found = matters::load_matter(workspace_path, id);
	if (!found) throw std::runtime_error("This project no longer exists.");
	const std::string timestamp = workspaces::current_timestamp();
	privacy::add_entity_alias(*found, entity_id, alias, timestamp);
	found->updated_at = timestamp;
	matters::save_matter(workspace_path, *found);
}

void remove_entity_alias(const std::string &id, const std::string &entity_id, const std::string &alias) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto found = matters::load_matter(workspace_path, id);
	if (!found) throw std::runtime_error("This project no longer exists.");
	const std::string timestamp = workspaces::current_timestamp();
	privacy::remove_entity_alias(*found, entity_id, alias, timestamp);
	found->updated_at = timestamp;
	matters::save_matter(workspace_path, *found);
}

} // namespace singularity::privacy_actions
