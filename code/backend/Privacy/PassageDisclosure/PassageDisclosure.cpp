#include "Privacy/PassageDisclosure/PassageDisclosure.hpp"
#include "Privacy/PassageDisclosure/RelationReading.hpp"

#include "Document/DocumentCache/DocumentCache.hpp"
#include "Document/DocumentChunker/DocumentChunker.hpp"
#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Document/DocumentJournal/DocumentJournal.hpp"
#include "Document/DocumentSourceResolver/DocumentSourceResolver.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Privacy/ContextRecords/ContextRecords.hpp"
#include "Privacy/EntityRegistry/EntityRegistry.hpp"
#include "Routing/RoutingLog/RoutingLog.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace singularity::privacy::passage_disclosure {
namespace {

// Everything one disclosure is read from, loaded once.
struct Loaded {
	matters::Matter project;
	// The operation's own task, when it is one of the Project's.
	std::optional<matters::MatterTask> task;
	// The operation's model calls, in the order made.
	std::vector<protected_api::papinian::OutboundCall> calls;
	routing::SentPassages sent;
	std::vector<sent_passages::MarkerFact> facts;
};

// The entity an id names now: one merged into another is read as the one it
// joined.
std::optional<ProjectEntity> current_entity(const matters::Matter &project, const std::string &entity_id) {
	std::optional<ProjectEntity> entity = find_entity(project, entity_id);
	for (int step = 0; step < 8 && entity.has_value() == true && entity->merged_into_entity_id.has_value() == true; ++step)
		entity = find_entity(project, *entity->merged_into_entity_id);
	return entity;
}

Loaded load(const std::string &project_id, const std::string &operation_id) {
	Loaded loaded;
	const std::filesystem::path workspace = node::require_active_workspace_path();
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		std::optional<matters::Matter> project = matters::load_matter(workspace, project_id);
		if (project.has_value() == false) throw std::runtime_error("This Project is no longer available.");
		loaded.project = std::move(*project);
	}
	for (const matters::MatterTask &task : loaded.project.tasks) {
		if (task.id != operation_id) continue;
		loaded.task = task;
		break;
	}
	for (const routing::RoutingLogEntry &entry : routing::load_routing_log(workspace)) {
		if (entry.matter_id != project_id || entry.operation_id != operation_id) continue;
		for (const protected_api::papinian::OutboundCall &call : entry.privacy_metadata.outbound_calls)
			if (call.kind == "model") loaded.calls.push_back(call);
	}
	// A record is read only for the Project it was written for.
	loaded.sent = routing::load_sent_passages(workspace, project_id, operation_id);
	loaded.facts = sent_passages::marker_facts(loaded.calls);
	for (sent_passages::MarkerFact &fact : loaded.facts) {
		if (fact.entity_id.empty() == true) continue;
		const std::optional<ProjectEntity> entity = current_entity(loaded.project, fact.entity_id);
		if (entity.has_value() == false) continue;
		fact.entity_name = entity->canonical_value;
		fact.aliases = entity->aliases;
	}
	return loaded;
}

// "doc_1:<fingerprint>:0-812" names the document "doc_1".
std::string document_of(const std::string &chunk_id) {
	const std::size_t colon = chunk_id.find(':');
	return colon == std::string::npos ? std::string() : chunk_id.substr(0, colon);
}

std::string latest_time(const routing::SentPassage &passage) {
	std::string latest;
	for (const protected_api::papinian::OutboundCall &call : passage.calls) latest = std::max(latest, call.time);
	return latest;
}

/* - - - - - - */

// What the Project remembers went into a request one record a line; it is
// found again from the Project's own records (Privacy/ContextRecords).
using context_records::ProjectValues;
using context_records::add_value;

std::string folder_of(const std::string &relative_path) {
	return std::filesystem::path(relative_path).parent_path().generic_string();
}

ProjectValues project_values(const matters::Matter &project) {
	ProjectValues values;
	context_records::ValuePool &pool = values.pool;
	for (std::string line : documents::journal::records_for_request(project.document_events,
			std::numeric_limits<std::size_t>::max())) {
		const nlohmann::ordered_json record = nlohmann::ordered_json::parse(line, nullptr, false);
		if (record.is_discarded() == false) context_records::add_record_values(pool, record, std::string());
		// A request is given the line without its line feed (OutboundRecord's
		// add_context_lines digests it so).
		if (line.empty() == false && line.back() == '\n') line.pop_back();
		values.lines.push_back(std::move(line));
	}
	for (const matters::MatterDocument &document : project.documents) {
		for (const char *key : {"name", "document", "firstNames", "previousName"}) add_value(pool, key, document.name);
		const std::string folder = folder_of(document.relative_path);
		for (const char *key : {"folder", "previousFolder"}) add_value(pool, key, folder);
	}
	for (const documents::DocumentEvent &event : project.document_events) {
		for (const char *key : {"name", "document", "firstNames", "previousName"}) {
			add_value(pool, key, event.name);
			add_value(pool, key, event.previous_name);
		}
		for (const char *key : {"folder", "previousFolder"}) {
			add_value(pool, key, folder_of(event.relative_path));
			add_value(pool, key, folder_of(event.previous_relative_path));
		}
	}
	for (const context::ContextItem &item : project.context_items) {
		add_value(pool, "label", item.label);
		add_value(pool, "value", item.value);
	}
	return values;
}

struct LocalPassage {
	std::optional<std::string> text;
	bool is_changed = false;
	std::string document_name;
};

// Reads a recorded passage beside the Project's text. Each local passage is
// resolved once per disclosure, through the one source resolver the source
// peek uses.
class Reader {
public:
	Reader(const std::string &project_id, const Loaded &loaded)
		: projectId_(project_id)
		, loaded_(loaded)
	{
	}

	LocalPassage local(const std::string &document_id, const std::string &chunk_id) {
		const auto cached = cache_.find(chunk_id);
		if (cached != cache_.end()) return cached->second;
		LocalPassage passage;
		std::string reading;
		if (loaded_.task.has_value() == true) {
			for (const context::ResolvedSourceReference &reference : loaded_.task->source_references) {
				if (reference.chunk_id != chunk_id) continue;
				reading = reference.extractor_version;
				passage.document_name = reference.document_name;
				break;
			}
		}
		for (const matters::MatterDocument &document : loaded_.project.documents)
			if (document.id == document_id) passage.document_name = document.name;
		try {
			const document::ResolvedSource resolved = document::resolve_source(projectId_, document_id, chunk_id, reading);
			if (resolved.text_preview.empty() == false && resolved.chunk_id == chunk_id) passage.text = resolved.text_preview;
			passage.is_changed = resolved.status == document::SourceResolutionStatus::Changed;
			if (resolved.document_name.empty() == false) passage.document_name = resolved.document_name;
		} catch (const std::exception &) {
			passage.text.reset();
		}
		cache_.emplace(chunk_id, passage);
		return passage;
	}

	// The document passage a recorded one was sent from: the one it names, or
	// - for a passage sent in a rendering that did not name it - the cited
	// passage whose text it was built from.
	std::pair<std::string, std::string> place(const routing::SentPassage &passage) {
		if (passage.chunk_id.empty() == false) return {document_of(passage.chunk_id), passage.chunk_id};
		if (loaded_.task.has_value() == false) return {};
		std::set<std::string> tried;
		for (const context::ResolvedSourceReference &reference : loaded_.task->source_references) {
			if (reference.chunk_id.empty() == true || tried.insert(reference.chunk_id).second == false) continue;
			const LocalPassage candidate = local(reference.document_id, reference.chunk_id);
			if (candidate.text.has_value() == false) continue;
			if (sent_passages::built_from(passage.content_sha256, *candidate.text).has_value() == true)
				return {reference.document_id, reference.chunk_id};
		}
		// A passage the answer did not cite: every passage of the documents the
		// request was supplied, as Singularity reads them now.
		for (const documents::DocumentChunk &chunk : supplied_chunks()) {
			if (tried.count(chunk.chunk_id) > 0) continue;
			if (sent_passages::built_from(passage.content_sha256, chunk.text).has_value() == true)
				return {chunk.document_id, chunk.chunk_id};
		}
		return {};
	}

	// The passages of every document the request was supplied, read once per
	// disclosure from Singularity's own reading of each (a document not ready now
	// has none).
	const std::vector<documents::DocumentChunk> &supplied_chunks() {
		if (chunks_.has_value() == true) return *chunks_;
		chunks_.emplace();
		if (loaded_.task.has_value() == false) return *chunks_;
		try {
			const documents::LocalDocumentExtractor extractor;
			const documents::DocumentCache cache(node::require_active_workspace_path(), extractor);
			for (const matters::SourceCoverage &source : loaded_.task->request_coverage.sources) {
				if (source.supplied == 0) continue;
				for (const matters::MatterDocument &document : loaded_.project.documents) {
					if (document.id != source.document_id || document.available == false
							|| document.lifecycle_state != matters::DocumentLifecycleState::Ready) continue;
					const std::optional<documents::DocumentExtractionResult> extraction =
						cache.load(document.fingerprint, document.extractor_version);
					if (extraction.has_value() == false || extraction->ai_ready() == false) continue;
					for (documents::DocumentChunk &chunk : documents::chunk_document(document.id, document.fingerprint, *extraction))
						chunks_->push_back(std::move(chunk));
				}
			}
		} catch (const std::exception &) {
		}
		return *chunks_;
	}

	DisclosedPassage read(const routing::SentPassage &passage, const std::vector<sent_passages::MarkerFact> &facts) {
		return read(passage, facts, passage.kind == "source" ? place(passage) : std::pair<std::string, std::string>());
	}

	// The same, for a passage already known to be `placed` (document, passage).
	DisclosedPassage read(const routing::SentPassage &passage, const std::vector<sent_passages::MarkerFact> &facts,
			const std::pair<std::string, std::string> &placed) {
		DisclosedPassage disclosed;
		disclosed.kind = passage.kind;
		disclosed.calls = passage.calls;
		std::optional<std::string> text;
		if (passage.kind == "context") {
			const std::vector<sent_passages::MarkerFact> scoped = sent_passages::facts_in_scope(facts, passage.symbol_scope);
			if (values_.has_value() == false) values_ = project_values(loaded_.project);
			const context_records::Rebuilt rebuilt = context_records::rebuild_record(passage.sent, passage.content_sha256, *values_, scoped);
			if (rebuilt.line.has_value() == true) {
				sent_passages::PassageReading reading = sent_passages::read_passage(passage.sent, passage.content_sha256, rebuilt.line, scoped);
				disclosed.reading = reading.reading;
				disclosed.sent = std::move(reading.sent);
				disclosed.local = std::move(reading.local);
				disclosed.hidden = std::move(reading.hidden);
				return disclosed;
			}
			// What the Project remembers no longer makes this record: it has
			// changed since (a document renamed, a Context item edited). A search
			// cut short proves no change: its values are only not lined up.
			disclosed.reading = rebuilt.is_cut == true ? sent_passages::Reading::Unaligned : sent_passages::Reading::Changed;
			disclosed.sent = sent_passages::sent_segments(passage.sent, scoped);
			disclosed.hidden = sent_passages::hidden_values(disclosed.sent, {}, scoped);
			return disclosed;
		}
		if (passage.kind == "instruction") {
			if (loaded_.task.has_value() == true) text = loaded_.task->prompt;
		} else {
			disclosed.document_id = placed.first;
			disclosed.chunk_id = placed.second;
			if (placed.second.empty() == false) {
				const LocalPassage found = local(placed.first, placed.second);
				text = found.text;
				disclosed.is_document_changed = found.is_changed;
				disclosed.document_name = found.document_name;
			}
		}
		// A passage's markers name values only within the table it was sent under.
		sent_passages::PassageReading reading = sent_passages::read_passage(passage.sent, passage.content_sha256, text,
			sent_passages::facts_in_scope(facts, passage.symbol_scope));
		disclosed.reading = reading.reading;
		disclosed.sent = std::move(reading.sent);
		disclosed.local = std::move(reading.local);
		disclosed.hidden = std::move(reading.hidden);
		return disclosed;
	}

private:
	const std::string &projectId_;
	const Loaded &loaded_;
	std::map<std::string, LocalPassage> cache_;
	std::optional<ProjectValues> values_;
	std::optional<std::vector<documents::DocumentChunk>> chunks_;
};

// The lawyer's own instruction is the instruction passage built from the
// task's instruction; Singularity's own requests to a reviewer are not it.
bool is_lawyers_instruction(const routing::SentPassage &passage, const Loaded &loaded) {
	return passage.kind == "instruction" && loaded.task.has_value() == true
		&& sent_passages::built_from(passage.content_sha256, loaded.task->prompt).has_value() == true;
}

// Every document passage read, then read again where it was ambiguous with
// what the others showed each value as.
std::vector<DisclosedPassage> read_sources(Reader &reader, const Loaded &loaded,
		std::vector<sent_passages::MarkerFact> &facts, std::vector<sent_passages::HiddenValue> &hidden) {
	std::vector<DisclosedPassage> passages;
	for (const routing::SentPassage &passage : loaded.sent.passages) {
		if (passage.kind != "source") continue;
		passages.push_back(reader.read(passage, facts));
		sent_passages::merge_hidden_values(hidden, passages.back().hidden);
	}
	sent_passages::learn_spellings(facts, hidden);
	std::size_t index = 0;
	for (const routing::SentPassage &passage : loaded.sent.passages) {
		if (passage.kind != "source") continue;
		DisclosedPassage &disclosed = passages.at(index++);
		if (disclosed.reading != sent_passages::Reading::Ambiguous) continue;
		disclosed = reader.read(passage, facts);
		sent_passages::merge_hidden_values(hidden, disclosed.hidden);
	}
	return passages;
}

// Every record of what the Project remembers that left with a marker, read
// beside the Project's values; each adds the originals it lines up with.
std::vector<DisclosedPassage> read_contexts(Reader &reader, const Loaded &loaded,
		std::vector<sent_passages::MarkerFact> &facts, std::vector<sent_passages::HiddenValue> &hidden) {
	std::vector<DisclosedPassage> passages;
	for (const routing::SentPassage &passage : loaded.sent.passages) {
		if (passage.kind != "context") continue;
		passages.push_back(reader.read(passage, facts));
		sent_passages::merge_hidden_values(hidden, passages.back().hidden);
	}
	sent_passages::learn_spellings(facts, hidden);
	return passages;
}

// Why a value is shown by its label alone: what became of the passages that
// carried it, or that none Singularity keeps did.
void explain_missing(std::vector<sent_passages::HiddenValue> &hidden, const std::vector<const DisclosedPassage *> &read) {
	for (sent_passages::HiddenValue &value : hidden) {
		value.missing.clear();
		if (value.originals.empty() == false || value.entity_name.empty() == false) continue;
		bool isCarried = false;
		bool isChanged = false;
		bool isUnavailable = false;
		for (const DisclosedPassage *passage : read) {
			bool carries = false;
			for (const sent_passages::Segment &segment : passage->sent) {
				if (segment.is_hidden == false) continue;
				if (value.label.empty() == false ? segment.label == value.label : segment.marker == value.marker) carries = true;
			}
			if (carries == false) continue;
			isCarried = true;
			if (passage->reading == sent_passages::Reading::Changed) isChanged = true;
			if (passage->reading == sent_passages::Reading::Unavailable) isUnavailable = true;
		}
		if (isCarried == false) value.missing = "not_kept";
		else if (isChanged == true) value.missing = "changed";
		else if (isUnavailable == true) value.missing = "unavailable";
		else value.missing = "unmatched";
	}
}

} // namespace

namespace {

// The relations the operation's requests carried, each read again on this
// device: every symbol's original from the operation's hidden values (the
// table's own facts), a worked-out value by working it out again.
void add_relations(OperationDisclosure &disclosure, const Loaded &loaded,
		const std::vector<sent_passages::MarkerFact> &facts) {
	for (const protected_api::papinian::OutboundCall &call : loaded.calls) {
		disclosure.withheld_relations += call.withheld_relations.size();
		if (call.relations.empty() == true) continue;
		const std::vector<sent_passages::MarkerFact> scoped = sent_passages::facts_in_scope(facts, call.symbol_scope);
		std::map<std::string, std::string> originals;
		for (const sent_passages::MarkerFact &fact : scoped)
			for (const sent_passages::HiddenValue &value : disclosure.hidden)
				if (value.label == fact.label && value.originals.empty() == false) {
					originals.emplace(fact.marker, value.originals.front());
					break;
				}
		for (const protected_api::papinian::OutboundRelation &relation : call.relations) {
			DisclosedRelation disclosed;
			disclosed.id = relation.id;
			disclosed.kind = relation.kind;
			disclosed.sent = relation.sent;
			disclosed.local = local_reading(relation, originals);
			disclosed.why = relation_reason(relation.justification);
			disclosed.sensitivity = relation.relation_sensitivity;
			disclosed.time = call.time;
			disclosed.provider_id = call.provider_id;
			disclosed.model_id = call.model_id;
			disclosure.relations.push_back(std::move(disclosed));
		}
	}
}

// Every symbol's original for one call's numbering, from the operation's
// hidden values (the table's own facts).
std::map<std::string, std::string> originals_for(const OperationDisclosure &disclosure,
		const std::vector<sent_passages::MarkerFact> &facts, const std::string &symbol_scope) {
	std::map<std::string, std::string> originals;
	for (const sent_passages::MarkerFact &fact : sent_passages::facts_in_scope(facts, symbol_scope))
		for (const sent_passages::HiddenValue &value : disclosure.hidden)
			if (value.label == fact.label && value.originals.empty() == false) {
				originals.emplace(fact.marker, value.originals.front());
				break;
			}
	return originals;
}

// The protected computation protocol's part of what the requests carried
// (DESIGN4 §11.3): role lines and table blocks, each once, and each operation
// the model asked for with the line it was sent back, read here with the
// originals - worked out again where a value was worked out.
void add_computation(OperationDisclosure &disclosure, const Loaded &loaded,
		const std::vector<sent_passages::MarkerFact> &facts) {
	std::set<std::string> rolesSeen;
	std::set<std::string> tablesSeen;
	std::map<std::string, std::size_t> operationAt;
	std::map<std::string, std::map<std::string, std::string>> originalsByScope;
	const auto originals = [&](const std::string &scope) -> std::map<std::string, std::string> & {
		auto found = originalsByScope.find(scope);
		if (found == originalsByScope.end()) found = originalsByScope.emplace(scope, originals_for(disclosure, facts, scope)).first;
		return found->second;
	};
	const auto callOf = [&](const std::string &payload) -> const protected_api::papinian::OutboundCall * {
		if (payload.empty() == true) return nullptr;
		for (const protected_api::papinian::OutboundCall &call : loaded.calls)
			if (call.payload_sha256 == payload) return &call;
		return nullptr;
	};
	const auto operationOf = [&](const protected_api::papinian::OutboundOperation &operation) -> DisclosedOperation & {
		const auto found = operationAt.find(operation.op_id);
		if (found != operationAt.end()) return disclosure.operations[found->second];
		DisclosedOperation disclosed;
		disclosed.id = operation.op_id;
		disclosed.requested = operation.requested;
		disclosed.status = operation.status;
		disclosed.code = operation.code;
		disclosed.observation = operation.observation;
		disclosed.sensitivity = operation.relation_sensitivity;
		disclosed.why = operation.justification;
		if (const protected_api::papinian::OutboundCall *asked = callOf(operation.asked_in_reply_to)) {
			disclosed.asked_time = asked->time;
			disclosed.asked_provider_id = asked->provider_id;
			disclosed.asked_model_id = asked->model_id;
		}
		operationAt.emplace(operation.op_id, disclosure.operations.size());
		disclosure.operations.push_back(std::move(disclosed));
		return disclosure.operations.back();
	};
	for (const protected_api::papinian::OutboundCall &call : loaded.calls) {
		for (const protected_api::papinian::OutboundRole &role : call.roles) {
			if (rolesSeen.insert(call.symbol_scope + "|" + role.line).second == false) continue;
			protected_api::papinian::OutboundRelation line;
			line.sent = role.line;
			DisclosedRole disclosed;
			disclosed.symbol = role.symbol;
			disclosed.line = role.line;
			disclosed.local = local_reading(line, originals(call.symbol_scope));
			disclosed.role_class = role.role_class;
			disclosed.sensitivity = role.relation_sensitivity;
			disclosed.time = call.time;
			disclosed.provider_id = call.provider_id;
			disclosed.model_id = call.model_id;
			disclosure.roles.push_back(std::move(disclosed));
		}
		for (const protected_api::papinian::OutboundTable &table : call.tables) {
			if (tablesSeen.insert(call.symbol_scope + "|" + table.block).second == false) continue;
			disclosure.tables.push_back({table.handle, table.block, table.justification, call.time, call.provider_id, call.model_id});
		}
		for (const protected_api::papinian::OutboundOperation &operation : call.operations_requested) operationOf(operation);
		for (const protected_api::papinian::OutboundOperation &operation : call.operations_returned) {
			DisclosedOperation &disclosed = operationOf(operation);
			// Every later call that sends the results block again carries the same
			// line: the lawyer reads the call it first went back to the model with.
			if (disclosed.time.empty() == false) continue;
			disclosed.returned = operation.returned;
			disclosed.status = operation.status;
			disclosed.code = operation.code;
			disclosed.local = local_operation_reading(operation.returned, originals(call.symbol_scope));
			disclosed.time = call.time;
			disclosed.provider_id = call.provider_id;
			disclosed.model_id = call.model_id;
		}
	}
}

} // namespace

OperationDisclosure disclose_operation(const std::string &project_id, const std::string &operation_id) {
	OperationDisclosure disclosure;
	disclosure.operation_id = operation_id;
	const Loaded loaded = load(project_id, operation_id);
	std::vector<sent_passages::MarkerFact> facts = loaded.facts;
	disclosure.hidden = sent_passages::fact_values(facts);
	if (loaded.sent.is_recorded == false) {
		explain_missing(disclosure.hidden, {});
		add_relations(disclosure, loaded, facts);
		add_computation(disclosure, loaded, facts);
		return disclosure;
	}
	disclosure.is_recorded = true;

	Reader reader(project_id, loaded);
	disclosure.passages = read_sources(reader, loaded, facts, disclosure.hidden);
	const std::vector<DisclosedPassage> contexts = read_contexts(reader, loaded, facts, disclosure.hidden);
	// A source passage that could not be told apart alone may be now, with
	// what the Project's records showed each value as.
	for (std::size_t index = 0, sourceIndex = 0; index < loaded.sent.passages.size(); ++index) {
		const routing::SentPassage &passage = loaded.sent.passages[index];
		if (passage.kind != "source") continue;
		DisclosedPassage &disclosed = disclosure.passages.at(sourceIndex++);
		if (contexts.empty() == true || disclosed.reading != sent_passages::Reading::Ambiguous) continue;
		disclosed = reader.read(passage, facts);
		sent_passages::merge_hidden_values(disclosure.hidden, disclosed.hidden);
	}
	const routing::SentPassage *instruction = nullptr;
	for (const routing::SentPassage &passage : loaded.sent.passages) {
		if (is_lawyers_instruction(passage, loaded) == false) continue;
		if (instruction == nullptr || latest_time(passage) >= latest_time(*instruction)) instruction = &passage;
	}
	if (instruction != nullptr) {
		disclosure.instruction = reader.read(*instruction, facts);
		sent_passages::merge_hidden_values(disclosure.hidden, disclosure.instruction->hidden);
	}
	std::vector<const DisclosedPassage *> read;
	for (const DisclosedPassage &passage : disclosure.passages) read.push_back(&passage);
	for (const DisclosedPassage &passage : contexts) read.push_back(&passage);
	if (disclosure.instruction.has_value() == true) read.push_back(&*disclosure.instruction);
	explain_missing(disclosure.hidden, read);
	for (DisclosedPassage &passage : disclosure.passages) explain_missing(passage.hidden, {&passage});
	if (disclosure.instruction.has_value() == true) explain_missing(disclosure.instruction->hidden, {&*disclosure.instruction});
	add_relations(disclosure, loaded, facts);
	add_computation(disclosure, loaded, facts);
	return disclosure;
}

PassageDisclosure disclose_passage(const std::string &project_id, const std::string &operation_id,
		const std::string &document_id, const std::string &chunk_id) {
	PassageDisclosure disclosure;
	const Loaded loaded = load(project_id, operation_id);
	if (loaded.sent.is_recorded == false) return disclosure;

	Reader reader(project_id, loaded);
	const LocalPassage local = reader.local(document_id, chunk_id);
	const routing::SentPassage *chosen = nullptr;
	// A kept passage that names no passage is found only by its local text.
	bool isUnnamedKept = false;
	for (const routing::SentPassage &passage : loaded.sent.passages) {
		if (passage.kind != "source") continue;
		if (passage.chunk_id.empty() == true) isUnnamedKept = true;
		const bool isNamed = passage.chunk_id == chunk_id;
		const bool isBuiltFrom = passage.chunk_id.empty() == true && local.text.has_value() == true
			&& sent_passages::built_from(passage.content_sha256, *local.text).has_value() == true;
		if (isNamed == false && isBuiltFrom == false) continue;
		++disclosure.form_count;
		if (chosen == nullptr || latest_time(passage) >= latest_time(*chosen)) chosen = &passage;
	}
	if (chosen == nullptr) {
		// With no local text to find it by, a passage that went unnamed may be
		// among what was kept: that is said, never that it was not kept.
		disclosure.state = local.text.has_value() == false && isUnnamedKept == true ? "unavailable" : "not_kept";
		return disclosure;
	}
	disclosure.state = "recorded";
	std::vector<sent_passages::MarkerFact> facts = loaded.facts;
	const std::pair<std::string, std::string> placed(document_id, chunk_id);
	DisclosedPassage passage = reader.read(*chosen, facts, placed);
	// A passage whose markers cannot be told apart alone may be told apart by
	// how the operation's other passages show the same values.
	if (passage.reading == sent_passages::Reading::Ambiguous) {
		std::vector<sent_passages::HiddenValue> hidden = sent_passages::fact_values(facts);
		(void)read_sources(reader, loaded, facts, hidden);
		passage = reader.read(*chosen, facts, placed);
	}
	if (passage.document_name.empty() == true) passage.document_name = local.document_name;
	explain_missing(passage.hidden, {&passage});
	disclosure.passage = std::move(passage);
	return disclosure;
}

} // namespace singularity::privacy::passage_disclosure
