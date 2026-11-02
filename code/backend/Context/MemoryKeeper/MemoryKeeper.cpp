#include "Context/MemoryKeeper/MemoryKeeper.hpp"

#include "Context/ContextCuration/ContextCuration.hpp"
#include "Context/ContextRegistry/ContextRegistry.hpp"
#include "Context/SourceMarkers/SourceMarkers.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "PapinianGuard/Taxonomy/Taxonomy.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Privacy/EntityRegistry/EntityRegistry.hpp"
#include "Platform/AppLifecycle/AppLifecycle.hpp"
#include "Platform/Trace/Trace.hpp"
#include "Usage/LocalWork/LocalWork.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace singularity::memory_keeper {
namespace {

// 600 words of legal prose, with room for the names and figures it carries.
constexpr std::size_t kBriefBytes = 6000;
constexpr std::size_t kDigestBytes = 900;
// What of a document is read for its digest: its opening - who, what, when -
// and as much of what follows as this computer's model reads in about half a
// minute. The first document is read at the small bound; every next one at
// what the last one's speed allows (next_document_bytes).
constexpr std::size_t kDocumentBytesMinimum = 3000;
constexpr std::size_t kDocumentBytesMaximum = 24000;
constexpr double kDocumentSecondsTarget = 30.0;
constexpr std::size_t kChatPromptBytes = 2000;
constexpr std::size_t kChatAnswerBytes = 8000;
// Progress is saved as it is made, so closing the app mid-way loses little.
constexpr std::size_t kDigestsPerSave = 4;

// Why the memory does not move, said the way it is. The keeper is started once
// Singularity has finished getting ready; until then no model can be asked, even
// one that is chosen - and saying "no model is chosen" then would send the
// lawyer to fix what is not broken.
constexpr const char *kNotReadyYet = "Memory starts keeping itself once Singularity has finished starting up.";
// With the keeper running, no model can be had: none is chosen for Context and
// no local model is installed - or the one chosen cannot be used now. Both are
// put right in the same place.
constexpr const char *kNoModelNow = "No model can keep this Project's memory right now. Choose one for Context in "
	"Models - or, if one is chosen, check that it is still installed or connected - and Memory keeps itself from "
	"the next chat.";

struct Job {
	enum class Kind { Chat, Documents } kind = Kind::Chat;
	std::string project_id;
	std::string task_id;
	bool everything = false;
};

struct Shared {
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<Job> jobs;
	Dependencies dependencies;
	bool started = false;
	std::atomic<bool> stopping{false};
	// One rewrite at a time, whoever asks for it.
	std::mutex rewrite;
	std::map<std::string, Status> progress;
};

// Leaked on purpose: the worker may be inside a local generation when the
// application ends, and must never find its state destroyed under it.
Shared &shared() {
	static Shared *instance = new Shared();
	return *instance;
}

std::string trimmed(const std::string &text) {
	const auto first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return {};
	const auto last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

std::string without_thinking(const std::string &text) {
	const auto thought = text.rfind("</think>");
	return thought == std::string::npos ? text : text.substr(thought + 8);
}

bool is_brief(const context::ContextItem &item) {
	return item.label == context::kMemoryBriefLabel
		&& item.authority == context::ContextItemAuthority::AutomaticallyDerived;
}

void set_progress(const std::string &project_id, const std::function<void(Status &)> &change) {
	Shared &state = shared();
	const std::lock_guard<std::mutex> guard(state.mutex);
	change(state.progress[project_id]);
}

struct Snapshot {
	// The brief the rewrite builds on: empty when AI may not use the stored one
	// (kept from AI, or written before a value was withdrawn: Beta 1 #77).
	std::string brief;
	// The brief as it was stored when the rewrite began. The rewrite is written
	// only while the stored brief is still exactly this (write_project).
	std::string stored_brief;
	std::string other_entries;
	// How this Project itself writes the names of the people and organisations
	// in it - the Project's own entity register, not the keeper's recollection
	// of them. A brief that calls a party "the vendor", "the lawyer" or a
	// shortened form of its name is a brief a partner does not trust, and a
	// model asked to summarise will produce exactly that unless it is told the
	// spellings it must use.
	std::string identity_register;
	std::optional<matters::MatterTask> task;
	std::vector<matters::MatterDocument> readable_documents;
	std::vector<matters::MemoryDocumentDigest> digests;
};

matters::Matter require_project(const std::vector<matters::Matter> &projects, const std::string &project_id) {
	const auto found = std::find_if(projects.begin(), projects.end(),
		[&](const matters::Matter &project) { return project.id == project_id; });
	if (found == projects.end()) throw std::runtime_error("This Project no longer exists.");
	return *found;
}

Snapshot read_project(const std::string &project_id, const std::string &task_id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const matters::Matter project = require_project(
		matters::load_matters(node::require_active_workspace_path()), project_id);
	Snapshot snapshot;
	for (const context::ContextItem &item : project.context_items) {
		if (is_brief(item)) {
			snapshot.stored_brief = item.value;
			if (context::context_item_standing(project, item).used_by_ai) snapshot.brief = item.value;
			continue;
		}
		if (item.is_excluded_from_ai) continue;
		if (snapshot.other_entries.size() < 6000) {
			snapshot.other_entries += "- " + item.label + ": " + bounded_text(item.value, 400) + "\n";
		}
	}
	// The names, longest writing first, so the register's own first spelling is
	// the full one a brief should use.
	for (const privacy::ProjectEntity &entity : privacy::list_active_entities(project)) {
		if (snapshot.identity_register.size() >= 3000) break;
		std::vector<std::string> writings = privacy::protection_values(project, entity);
		std::sort(writings.begin(), writings.end(),
			[](const std::string &left, const std::string &right) { return left.size() > right.size(); });
		writings.erase(std::unique(writings.begin(), writings.end()), writings.end());
		if (writings.empty()) continue;
		snapshot.identity_register += "- " + writings.front() + " ("
			+ protected_api::papinian::to_string(entity.category);
		if (writings.size() > 1) {
			snapshot.identity_register += "; also written";
			for (std::size_t index = 1; index < writings.size() && index < 5; ++index)
				snapshot.identity_register += (index == 1 ? ": " : ", ") + writings[index];
		}
		snapshot.identity_register += ")\n";
	}
	if (!task_id.empty()) {
		const auto task = std::find_if(project.tasks.begin(), project.tasks.end(),
			[&](const matters::MatterTask &candidate) { return candidate.id == task_id; });
		if (task != project.tasks.end()) snapshot.task = *task;
	}
	const bool real_project = project.directory_path && !project.directory_path->empty();
	for (const matters::MatterDocument &document : project.documents) {
		if (document.extraction_status != matters::DocumentExtractionState::Ready || !document.available) continue;
		if (real_project && document.source_mode != matters::DocumentSourceMode::Referenced) continue;
		snapshot.readable_documents.push_back(document);
	}
	snapshot.digests = project.memory_document_digests;
	return snapshot;
}

// Writes what the keeper arrived at. Both parts are optional, and nothing is
// written once the keeper was told to stop. Resolves whether the brief changed.
bool write_project(const std::string &project_id, const std::optional<std::string> &brief,
		context::ContextSourceKind source_kind, const std::string &task_id,
		const std::vector<matters::MemoryDocumentDigest> *digests, const std::string *expected_stored_brief = nullptr) {
	if (shared().stopping.load()) return false;
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace = node::require_active_workspace_path();
	std::vector<matters::Matter> projects = matters::load_matters(workspace);
	const auto project = std::find_if(projects.begin(), projects.end(),
		[&](const matters::Matter &candidate) { return candidate.id == project_id; });
	if (project == projects.end()) throw std::runtime_error("This Project no longer exists.");

	bool brief_changed = false;
	bool anything_changed = false;
	const std::string timestamp = workspaces::current_timestamp();
	if (brief) {
		const auto before = std::find_if(project->context_items.begin(), project->context_items.end(), is_brief);
		const std::string previous = before == project->context_items.end() ? std::string() : before->value;
		// The brief changed while the model was writing - cleared, edited, written
		// by another rewrite: what the model wrote from the old one is dropped,
		// never written over the new state (Beta 1 #77).
		if (expected_stored_brief != nullptr && previous != *expected_stored_brief) return false;
		context::ContextItemSource source;
		source.kind = source_kind;
		if (source_kind == context::ContextSourceKind::Chat && !task_id.empty()) source.chat_operation_id = task_id;
		const auto outcome = context::remember_from_chat(*project, context::ContextSection::KeyFacts,
			context::kMemoryBriefLabel, *brief, source, timestamp);
		const auto after = std::find_if(project->context_items.begin(), project->context_items.end(), is_brief);
		brief_changed = outcome.applied_directly && after != project->context_items.end() && after->value != previous;
		anything_changed = brief_changed;
	}
	if (digests != nullptr && *digests != project->memory_document_digests) {
		project->memory_document_digests = *digests;
		anything_changed = true;
	}
	if (!anything_changed) return false;
	project->updated_at = timestamp;
	matters::save_matters(workspace, projects);
	return brief_changed;
}

struct Generation {
	Keeper keeper;
	std::string text;
};

// What one piece of the keeper's work took, summed over its generations and
// recorded once when it ends (Usage/LocalWork).
struct Spent {
	Keeper keeper;
	long long input_tokens = 0;
	long long output_tokens = 0;
	providers::UsageMetadata usage;
	bool protected_api_applied = false;
	protected_api::papinian::PrivacyMetadata privacy_metadata;
	bool protection_seen = false;
};

// One generation's protection folded into the work's: the policy it ran
// under, everything it replaced, and a payload proof only when every call had one.
void add_protection(Spent &spent, const Generated &generated) {
	if (!spent.protection_seen) {
		spent.protected_api_applied = generated.protected_api_applied;
		spent.privacy_metadata = generated.privacy_metadata;
		spent.protection_seen = true;
		return;
	}
	protected_api::papinian::PrivacyMetadata &kept = spent.privacy_metadata;
	const protected_api::papinian::PrivacyMetadata &next = generated.privacy_metadata;
	spent.protected_api_applied = spent.protected_api_applied || generated.protected_api_applied;
	if (kept.mode == "off" && next.mode != "off") kept.mode = next.mode;
	kept.protected_count += next.protected_count;
	for (const auto &[category, count] : next.category_counts) kept.category_counts[category] += count;
	for (const std::string &entity : next.protected_project_entity_ids)
		if (std::find(kept.protected_project_entity_ids.begin(), kept.protected_project_entity_ids.end(), entity)
				== kept.protected_project_entity_ids.end())
			kept.protected_project_entity_ids.push_back(entity);
	kept.final_payload_validated = kept.final_payload_validated && next.final_payload_validated;
	if (!next.final_payload_sha256.empty()) kept.final_payload_sha256 = next.final_payload_sha256;
}

void record(const std::string &project_id, const char *label, const Spent &spent) {
	// Work that never reached a model - there was none to keep the memory - is
	// no operation: Usage would show a model-less entry for every chat.
	if (spent.keeper.model_id.empty() && spent.input_tokens == 0 && spent.output_tokens == 0) return;
	usage::LocalWork work;
	work.project_id = project_id;
	work.operation_type = usage::kLocalWorkMemory;
	work.label = label;
	work.model_id = spent.keeper.model_id;
	work.provider_id = spent.keeper.provider_id;
	work.execution_kind = spent.keeper.execution_kind;
	work.input_tokens = spent.input_tokens;
	work.output_tokens = spent.output_tokens;
	if (spent.usage.cost_micros || spent.usage.input_tokens || spent.usage.output_tokens) work.usage = spent.usage;
	work.protected_api_applied = spent.protected_api_applied;
	work.privacy_metadata = spent.privacy_metadata;
	usage::record_local_work(work);
}

Generation generate(const std::string &project_id, const std::string &prompt, Spent &spent) {
	Dependencies dependencies;
	bool ready = false;
	{
		const std::lock_guard<std::mutex> guard(shared().mutex);
		dependencies = shared().dependencies;
		ready = shared().started && !shared().stopping.load();
	}
	if (!ready) throw std::runtime_error(kNotReadyYet);
	const std::optional<Keeper> keeper = dependencies.choose_model ? dependencies.choose_model(project_id) : std::nullopt;
	if (!keeper || !dependencies.generate) throw std::runtime_error(kNoModelNow);
	Generated generated = dependencies.generate(project_id, *keeper, prompt);
	spent.keeper = *keeper;
	spent.input_tokens += generated.input_tokens;
	spent.output_tokens += generated.output_tokens;
	if (generated.usage.cost_micros || generated.usage.input_tokens || generated.usage.output_tokens)
		spent.usage = providers::accumulate_usage(spent.usage, generated.usage);
	add_protection(spent, generated);
	return Generation{*keeper, std::move(generated.text)};
}

// One rewrite of the brief from one piece of new material. Resolves the
// model's answer (for a message) and whether the brief changed.
Reply rewrite(const std::string &project_id, Material kind, const std::string &material,
		const std::string &task_id, const Snapshot &snapshot, Spent &spent) {
	platform::trace::Span span("memory", "rewrite", {{"source",
		kind == Material::Chat ? "chat" : kind == Material::Documents ? "documents" : "message"}});
	const Generation generation = generate(project_id,
		rewrite_prompt(snapshot.brief, snapshot.other_entries, snapshot.identity_register, kind, material), spent);
	const ParsedReply parsed = parse_reply(generation.text);
	Reply reply;
	reply.model_id = generation.keeper.model_id;
	reply.execution_kind = generation.keeper.execution_kind;
	reply.reply = parsed.reply;
	if (parsed.brief && parsed.brief->size() >= 20) {
		const context::ContextSourceKind source = kind == Material::Chat ? context::ContextSourceKind::Chat
			: kind == Material::Documents ? context::ContextSourceKind::Document : context::ContextSourceKind::User;
		reply.memory_changed = write_project(project_id, bounded_text(*parsed.brief, kBriefBytes), source, task_id, nullptr,
			&snapshot.stored_brief);
	}
	span.add("model", generation.keeper.model_id);
	span.add("execution", generation.keeper.execution_kind);
	span.add("memoryChanged", reply.memory_changed);
	return reply;
}

// The answer as the lawyer read it. Its source handles ("[SOURCE:DOC_1:CHUNK_2]")
// meant something only inside that one turn; left in, the brief copies them as
// "DOC_1: key document". Each becomes the name of the document it cited, and
// one the turn never cited is dropped rather than guessed at.
std::string readable_answer(const std::string &answer, const std::vector<context::ResolvedSourceReference> &cited) {
	context::RenderedSourceHandles rendered = context::render_source_handles(answer, cited,
		[](const context::ResolvedSourceReference &reference) { return "(" + reference.document_name + ")"; });
	std::string text = std::move(rendered.text);
	const std::string unverified = context::kUnverifiedSourceLabel;
	for (auto at = text.find(unverified); at != std::string::npos; at = text.find(unverified, at)) text.erase(at, unverified.size());
	return text;
}

void fold_chat(const Job &job, Spent &spent) {
	const Snapshot snapshot = read_project(job.project_id, job.task_id);
	if (!snapshot.task || !snapshot.task->response_text) return;
	const std::string material = "THE LAWYER ASKED:\n" + bounded_text(snapshot.task->prompt, kChatPromptBytes)
		+ "\n\nTHE ANSWER THEY WERE GIVEN:\n"
		+ bounded_text(readable_answer(*snapshot.task->response_text, snapshot.task->source_references), kChatAnswerBytes);
	(void)rewrite(job.project_id, Material::Chat, material, job.task_id, snapshot, spent);
}

void fold_documents(const Job &job, Spent &spent) {
	Snapshot snapshot = read_project(job.project_id, {});
	std::vector<matters::MemoryDocumentDigest> digests;
	// A digest belongs to a document that is still here.
	for (const matters::MemoryDocumentDigest &digest : snapshot.digests) {
		const bool still_here = std::any_of(snapshot.readable_documents.begin(), snapshot.readable_documents.end(),
			[&](const matters::MatterDocument &document) { return document.id == digest.document_id; });
		if (still_here) digests.push_back(digest);
	}
	std::vector<matters::MatterDocument> unread;
	for (const matters::MatterDocument &document : snapshot.readable_documents) {
		const auto known = std::find_if(digests.begin(), digests.end(),
			[&](const matters::MemoryDocumentDigest &digest) { return digest.document_id == document.id; });
		if (job.everything || known == digests.end() || known->fingerprint != document.fingerprint) unread.push_back(document);
	}
	set_progress(job.project_id, [&](Status &status) {
		status.documents_done = 0;
		status.documents_total = unread.size();
	});
	if (unread.empty() && digests == snapshot.digests) return;

	Dependencies dependencies;
	{
		const std::lock_guard<std::mutex> guard(shared().mutex);
		dependencies = shared().dependencies;
	}
	std::size_t since_save = 0;
	std::size_t document_bytes = kDocumentBytesMinimum;
	std::string fresh;
	for (const matters::MatterDocument &document : unread) {
		if (shared().stopping.load()) return;
		const std::string text = dependencies.read_document
			? dependencies.read_document(job.project_id, document.id, document_bytes) : std::string();
		std::string digest;
		if (!trimmed(text).empty()) {
			platform::trace::Span span("memory", "digest", {{"bytes", text.size()}});
			const auto started = std::chrono::steady_clock::now();
			try {
				digest = clean_digest(generate(job.project_id,
					digest_prompt(document.name, text, snapshot.identity_register), spent).text);
			} catch (const std::exception &error) {
				// One document that cannot be read now does not stop the others: it
				// stays unread, to be read on the next pass, and the pass says why.
				span.add("outcome", "failed");
				set_progress(job.project_id, [&](Status &status) {
					status.last_error = bounded_text(error.what(), 300);
					status.documents_done += 1;
				});
				continue;
			}
			const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
			document_bytes = next_document_bytes(text.size(), seconds);
		}
		digests.erase(std::remove_if(digests.begin(), digests.end(),
			[&](const matters::MemoryDocumentDigest &known) { return known.document_id == document.id; }), digests.end());
		// A document nothing could be taken from is still marked as read, as it
		// is now: it is not read again until it changes.
		digests.push_back(matters::MemoryDocumentDigest{document.id, document.fingerprint, digest,
			workspaces::current_timestamp()});
		if (!digest.empty()) fresh += "DOCUMENT: " + document.name + "\n" + digest + "\n\n";
		set_progress(job.project_id, [](Status &status) { status.documents_done += 1; });
		if (++since_save >= kDigestsPerSave) {
			(void)write_project(job.project_id, std::nullopt, context::ContextSourceKind::Document, {}, &digests);
			since_save = 0;
		}
	}
	(void)write_project(job.project_id, std::nullopt, context::ContextSourceKind::Document, {}, &digests);
	if (trimmed(fresh).empty() || shared().stopping.load()) return;
	// Reading documents takes minutes and touches no brief, so it holds nobody
	// up; only folding what was read into the brief takes the brief's turn -
	// and the brief may have moved meanwhile, so it is read again.
	const std::lock_guard<std::mutex> turn(shared().rewrite);
	snapshot = read_project(job.project_id, {});
	(void)rewrite(job.project_id, Material::Documents, bounded_text(fresh, 16000), {}, snapshot, spent);
}

void run(const Job &job) {
	// Its model calls stop on their connection once Singularity is closing
	// (Platform/AppLifecycle).
	const platform::lifecycle::ClosingCancelScope closingStops;
	const char *source = job.kind == Job::Kind::Chat ? "chat" : "documents";
	set_progress(job.project_id, [&](Status &status) {
		status.is_updating = true;
		status.source = source;
		status.last_error.clear();
	});
	std::string failure;
	Spent spent;
	try {
		if (job.kind == Job::Kind::Chat) {
			const std::lock_guard<std::mutex> turn(shared().rewrite);
			fold_chat(job, spent);
		} else {
			fold_documents(job, spent);
		}
	} catch (const std::exception &error) {
		failure = error.what();
	} catch (...) {
		failure = "Memory could not be updated.";
	}
	// What the work took is counted whether or not it arrived anywhere.
	record(job.project_id, job.kind == Job::Kind::Chat ? "Memory: a chat folded in" : "Memory: documents read", spent);
	set_progress(job.project_id, [&](Status &status) {
		status.is_updating = false;
		status.source.clear();
		status.documents_done = 0;
		status.documents_total = 0;
		if (!failure.empty()) status.last_error = failure;
	});
	if (!failure.empty()) platform::trace::record("memory", "failed", {{"source", source}, {"reason", bounded_text(failure, 200)}});
}

void work() {
	platform::trace::set_thread_label("memory-keeper");
	Shared &state = shared();
	while (true) {
		Job job;
		{
			std::unique_lock<std::mutex> lock(state.mutex);
			state.wake.wait(lock, [&] { return state.stopping.load() || !state.jobs.empty(); });
			if (state.stopping.load()) return;
			job = state.jobs.front();
			state.jobs.pop_front();
		}
		run(job);
	}
}

void enqueue(Job job) {
	Shared &state = shared();
	{
		const std::lock_guard<std::mutex> guard(state.mutex);
		if (!state.started || state.stopping.load()) return;
		// The same documents pass is not queued twice.
		if (job.kind == Job::Kind::Documents) {
			const bool queued = std::any_of(state.jobs.begin(), state.jobs.end(), [&](const Job &other) {
				return other.kind == Job::Kind::Documents && other.project_id == job.project_id
					&& (other.everything || !job.everything);
			});
			if (queued) return;
		}
		state.jobs.push_back(std::move(job));
	}
	state.wake.notify_one();
}

} // namespace

void start(Dependencies dependencies) {
	Shared &state = shared();
	const std::lock_guard<std::mutex> guard(state.mutex);
	state.dependencies = std::move(dependencies);
	if (state.started && !state.stopping.load()) return;
	state.started = true;
	state.stopping.store(false);
	std::thread(work).detach();
}

void stop() noexcept {
	Shared &state = shared();
	state.stopping.store(true);
	state.wake.notify_all();
}

void chat_answered(const std::string &project_id, const std::string &task_id) {
	if (project_id.empty() || task_id.empty()) return;
	enqueue(Job{Job::Kind::Chat, project_id, task_id, false});
}

void read_documents(const std::string &project_id, bool everything) {
	if (project_id.empty()) return;
	enqueue(Job{Job::Kind::Documents, project_id, {}, everything});
}

Reply tell(const std::string &project_id, const std::string &message) {
	const std::string said = trimmed(message);
	if (said.empty()) throw std::runtime_error("Say what to remember, or ask.");
	const std::lock_guard<std::mutex> turn(shared().rewrite);
	const Snapshot snapshot = read_project(project_id, {});
	Spent spent;
	Reply reply;
	try {
		reply = rewrite(project_id, Material::Message, bounded_text(said, 4000), {}, snapshot, spent);
	} catch (...) {
		record(project_id, "Memory: told by the lawyer", spent);
		throw;
	}
	record(project_id, "Memory: told by the lawyer", spent);
	if (reply.reply.empty()) reply.reply = reply.memory_changed ? "Done." : "Nothing in Memory changes.";
	return reply;
}

Status status(const std::string &project_id) {
	Status result;
	Dependencies dependencies;
	{
		Shared &state = shared();
		const std::lock_guard<std::mutex> guard(state.mutex);
		const auto known = state.progress.find(project_id);
		if (known != state.progress.end()) result = known->second;
		dependencies = state.dependencies;
		result.ready = state.started && !state.stopping.load();
	}
	const std::optional<Keeper> keeper = result.ready && dependencies.choose_model
		? dependencies.choose_model(project_id) : std::nullopt;
	result.model_available = keeper.has_value();
	if (keeper) {
		result.model_id = keeper->model_id;
		result.execution_kind = keeper->execution_kind;
	} else {
		// Without a model the memory cannot move, and why is the one thing to say
		// - which of the two reasons it is, never a guess, and never an older
		// failure that is no longer the reason.
		result.last_error = result.ready ? kNoModelNow : kNotReadyYet;
	}
	const Snapshot snapshot = read_project(project_id, {});
	result.documents_readable = snapshot.readable_documents.size();
	for (const matters::MatterDocument &document : snapshot.readable_documents) {
		const bool read = std::any_of(snapshot.digests.begin(), snapshot.digests.end(),
			[&](const matters::MemoryDocumentDigest &digest) {
				return digest.document_id == document.id && digest.fingerprint == document.fingerprint;
			});
		if (read) result.documents_read += 1;
	}
	return result;
}

// ---- Pure parts --------------------------------------------------------------------

std::size_t next_document_bytes(std::size_t bytes_read, double seconds_taken) {
	if (bytes_read == 0 || seconds_taken <= 0.0) return kDocumentBytesMinimum;
	const double bytes = static_cast<double>(bytes_read) / seconds_taken * kDocumentSecondsTarget;
	if (bytes <= static_cast<double>(kDocumentBytesMinimum)) return kDocumentBytesMinimum;
	if (bytes >= static_cast<double>(kDocumentBytesMaximum)) return kDocumentBytesMaximum;
	return static_cast<std::size_t>(bytes);
}

std::string bounded_text(const std::string &text, std::size_t maximum_bytes) {
	if (text.size() <= maximum_bytes) return text;
	std::size_t end = maximum_bytes;
	while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
	return text.substr(0, end);
}

ParsedReply parse_reply(const std::string &model_text) {
	const std::string text = without_thinking(model_text);
	ParsedReply parsed;
	const auto open = text.find('{');
	const auto close = text.rfind('}');
	if (open != std::string::npos && close != std::string::npos && close > open) {
		const nlohmann::json value = nlohmann::json::parse(text.substr(open, close - open + 1), nullptr, false);
		if (value.is_object() && (value.contains("reply") || value.contains("memoryBrief"))) {
			if (value.contains("reply") && value.at("reply").is_string()) parsed.reply = trimmed(value.at("reply").get<std::string>());
			if (value.contains("memoryBrief") && value.at("memoryBrief").is_string()) {
				std::string brief = trimmed(value.at("memoryBrief").get<std::string>());
				if (!brief.empty()) parsed.brief = std::move(brief);
			}
			return parsed;
		}
	}
	parsed.reply = trimmed(text);
	return parsed;
}

std::string clean_digest(const std::string &model_text) {
	return bounded_text(trimmed(without_thinking(model_text)), kDigestBytes);
}

// The rule that keeps names right. A brief is read by a partner, and a party
// named "the vendor" or written a word short of its real name is what makes
// them stop trusting it. The Project's own register decides every spelling; a
// role word is never a name.
std::string naming_rule(const std::string &identity_register) {
	if (trimmed(identity_register).empty()) {
		return "\n\nNAMES: name every party exactly as the material writes it. Never a role word (\"the vendor\", "
			"\"the lawyer\", \"the client\") where a name is available, and never a shortened or expanded form of "
			"your own.\n";
	}
	return "\n\nTHIS PROJECT'S OWN REGISTER OF NAMES - use these spellings and no others:\n" + identity_register
		+ "Name every party by the first spelling given for it above, even where the material writes it another way. "
		"Never a role word (\"the vendor\", \"the lawyer\", \"the client\") where the register has a name, and never "
		"a shortened or expanded form of your own. A party that is not in the register is named exactly as the "
		"material writes it.\n";
}

std::string rewrite_prompt(const std::string &current_brief, const std::string &other_entries,
		const std::string &identity_register, Material kind, const std::string &material) {
	std::string prompt =
		"You keep the memory of a legal Project for a lawyer: one brief of at most 600 words in plain Markdown, bullet "
		"points under a few short headings (such as Parties, Timeline, Documents, Risks, Decisions, Open points) - "
		"where the matter stands, who is who and whom we act for, the dates and amounts that matter, what the data "
		"room holds, what the lawyer decided or prefers, conclusions reached, open questions. Only what is known: "
		"never a placeholder for what is not, and never a point dropped unless it is wrong or stale.";
	prompt += naming_rule(identity_register);
	prompt += "\nCURRENT BRIEF:\n";
	prompt += trimmed(current_brief).empty() ? std::string("(empty)") : current_brief;
	prompt += "\n\nOTHER ENTRIES KEPT FOR THIS PROJECT:\n";
	prompt += trimmed(other_entries).empty() ? std::string("(none)\n") : other_entries;
	switch (kind) {
		case Material::Chat:
			prompt += "\nA CHAT IN THIS PROJECT JUST ARRIVED AT THIS.\n" + material
				+ "\n\nIf this changes what should be remembered, rewrite the WHOLE brief - merge, correct and drop what "
				"is stale, never just append - in the language the brief is written in (the lawyer's, for a first "
				"brief). Small talk and questions that settle nothing change nothing. Reply with one JSON object and "
				"nothing else: {\"memoryBrief\": \"the whole brief rewritten\" or null when nothing changes}.";
			break;
		case Material::Documents:
			prompt += "\nDOCUMENTS OF THE PROJECT'S DATA ROOM, AS JUST READ:\n" + material
				+ "\nFold into the brief what a lawyer on this matter must remember from them, and rewrite the WHOLE "
				"brief - merge, correct and drop what is stale, never just append - in the language the brief is "
				"written in (the documents', for a first brief). Reply with one JSON object and nothing else: "
				"{\"memoryBrief\": \"the whole brief rewritten\" or null when nothing changes}.";
			break;
		case Material::Message:
			prompt += "\nTHE LAWYER SAYS:\n" + material
				+ "\n\nAnswer in the lawyer's language. If the message changes what should be remembered (something to "
				"add, correct or forget), rewrite the WHOLE brief - merge and correct, never just append. Reply with "
				"one JSON object and nothing else: {\"reply\": \"one or two sentences - your answer, or what you "
				"changed\", \"memoryBrief\": \"the whole brief rewritten\" or null when nothing changes}.";
			break;
	}
	return prompt + "\n\n/no_think";
}

std::string digest_prompt(const std::string &document_name, const std::string &document_text,
		const std::string &identity_register) {
	return "You read one document of a legal Project's data room for the lawyer's memory. In at most 80 words, as "
		"three to six plain Markdown bullet points, say what this document is, who its parties are, its dates and "
		"amounts, and the terms or risks a lawyer on this matter must remember. Only what the text says, in the "
		"document's own language. Reply with the bullet points and nothing else."
		+ naming_rule(identity_register) + "\nDOCUMENT: " + document_name
		+ "\n\n" + document_text + "\n\n/no_think";
}

} // namespace singularity::memory_keeper
