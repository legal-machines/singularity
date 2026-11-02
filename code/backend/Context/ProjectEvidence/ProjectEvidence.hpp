#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Context/ContextRetriever/ContextRetriever.hpp"
#include "Context/SourceMarkers/SourceMarkers.hpp"
#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Platform/Sha256/Sha256.hpp"
#include "Tool/ToolBroker/ToolBroker.hpp"

#include <nlohmann/json.hpp>

// Read-only Project discovery, search and reading for ONE operation: what lets
// a model that was given some passages decide, in the middle of its work, that
// it needs more of the Project and get it - the way a developer's agent lists,
// searches and opens the files of a repository.
//
// No second retrieval system: a search is the Project's own relevance
// retrieval (matters::retrieve_document_context) with the model's words as the
// query, and a reading is the same retrieval of one document's every passage.
// Everything given is numbered in the operation's own SourceMarkerRegistry
// basis - the passages the request was assembled from first, whatever is found
// later appended after them - so a passage found here is cited exactly like one
// the request carried, and every marker already issued keeps its meaning.
//
// The request's source boundary holds for everything, names included: when
// the lawyer chose documents, only those are listed, searched or read, and a
// document outside the choice answers exactly as a document that does not
// exist. Each answer fits the room the model tool loop gives it - one total for
// the whole call, whatever it names - and a passage is given whole or not at
// all: a quotation is checked against exactly what the model was shown.
namespace singularity::context::project_evidence {

// One document as the model is told of it.
struct InventoryEntry {
	std::string handle;
	std::string document_id;
	std::string path;
	std::string name;
	bool readable = false;
	// Why it cannot be read, in plain words; empty when it can.
	std::string unreadable_reason;
};

// One thing Singularity did for the model: what it looked for or read, what it
// found and gave, and when - the operation's own observable record.
struct EvidenceStep {
	// "search", "read" or "list".
	std::string kind;
	// The words searched for, the documents read, or the folder listed.
	std::string subject;
	std::vector<std::string> document_ids;
	std::vector<std::string> document_names;
	std::size_t passages = 0;
	// Passages found but not given because the model's context had no room.
	std::size_t passages_left_out = 0;
	// "given", "nothing_found", "already_given", "no_room", "not_found",
	// "not_readable", "listed".
	std::string outcome;
	std::string started_at;
	std::string completed_at;
};

// How many passages one search may return: a search is a first look, and a
// model that needs a whole document reads it.
inline constexpr std::size_t kMaximumSearchPassages = 12;
// How many documents one reading may name.
inline constexpr std::size_t kMaximumDocumentsPerRead = 4;
// How long a search may be, in bytes.
inline constexpr std::size_t kMaximumSearchBytes = 400;

namespace detail {

inline std::string lowered(std::string text) {
	for (char &character : text) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	return text;
}

inline std::string trimmed(const std::string &value) {
	const std::size_t first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return std::string();
	const std::size_t last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

// A passage's words, whatever the bytes of the file around them: runs of white
// space read as one, the ends trimmed. The same passage read again after its
// document was saved unchanged hashes alike; one changed word does not.
inline std::string passage_text_hash(const std::string &text) {
	std::string normalized;
	normalized.reserve(text.size());
	bool isSpace = false;
	for (const char character : text) {
		if (character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f'
				|| character == '\v') {
			isSpace = true;
			continue;
		}
		if (isSpace && !normalized.empty()) normalized += ' ';
		isSpace = false;
		normalized += character;
	}
	return platform::sha256_hex_digest(normalized);
}

inline std::string unreadable_reason_for(const matters::MatterDocument &document) {
	switch (matters::document_lifecycle_state(document)) {
		case matters::DocumentLifecycleState::Ready:
		case matters::DocumentLifecycleState::Changed:
			break;
		case matters::DocumentLifecycleState::Processing:
			return "Singularity is still reading it";
		case matters::DocumentLifecycleState::Unsupported:
			return "Singularity cannot read this kind of file";
		case matters::DocumentLifecycleState::Unavailable:
			return "it is no longer in the Project folder";
		case matters::DocumentLifecycleState::NeedsAttention:
			return document.attention_detail.value_or("its text could not be read");
	}
	switch (document.extraction_status) {
		case matters::DocumentExtractionState::Ready: return std::string();
		case matters::DocumentExtractionState::OcrRequired: return "it is a scan with no text layer";
		case matters::DocumentExtractionState::Empty: return "it holds no text";
		case matters::DocumentExtractionState::Unsupported: return "Singularity cannot read this kind of file";
		case matters::DocumentExtractionState::Unreadable: return document.attention_detail.value_or("its text could not be read");
		case matters::DocumentExtractionState::NotExtracted: return "Singularity is still reading it";
	}
	return "its text could not be read";
}

} // namespace detail

class Session {
public:
	using TokenEstimate = std::function<std::size_t(std::string_view)>;
	using Changeable = std::function<bool(const matters::MatterDocument &)>;

	// `basis` is the request's own passages in the order its source markers were
	// issued from; `scope` the documents the lawyer chose (empty: the whole
	// Project may be read). `changeable` says which documents this request may
	// also propose to change, and `nameableDocumentIds` the ones it was already
	// told it may.
	Session(std::filesystem::path workspacePath, matters::Matter project, std::vector<std::string> scope,
			std::vector<RetrievedContextChunk> basis, TokenEstimate estimate, Changeable changeable = {},
			std::unordered_set<std::string> nameableDocumentIds = {})
		: workspacePath_(std::move(workspacePath))
		, project_(std::move(project))
		, scope_(std::move(scope))
		, basis_(std::move(basis))
		, requestBasisSize_(basis_.size())
		, estimate_(std::move(estimate))
		, changeable_(std::move(changeable))
		, nameable_(std::move(nameableDocumentIds))
	{
		for (const RetrievedContextChunk &passage : basis_) known_.insert(passage.chunk.chunk_id);
		// The inventory IS the operation's view of the Project: a document the
		// request may not read is not in it, so no answer below can name it.
		const bool hasProjectFolder = project_.directory_path.has_value() && !project_.directory_path->empty();
		std::vector<const matters::MatterDocument *> listed;
		for (const matters::MatterDocument &document : project_.documents) {
			if (!in_scope(document.id) || !document.available) continue;
			if (hasProjectFolder && document.source_mode != matters::DocumentSourceMode::Referenced) continue;
			listed.push_back(&document);
		}
		std::stable_sort(listed.begin(), listed.end(),
			[](const matters::MatterDocument *left, const matters::MatterDocument *right) {
				return path_of(*left) < path_of(*right);
			});
		for (const matters::MatterDocument *document : listed) {
			InventoryEntry entry;
			entry.handle = "F" + std::to_string(inventory_.size() + 1);
			entry.document_id = document->id;
			entry.path = path_of(*document);
			entry.name = document->name;
			entry.unreadable_reason = detail::unreadable_reason_for(*document);
			entry.readable = entry.unreadable_reason.empty();
			inventory_.push_back(std::move(entry));
		}
	}

	// The passages the request itself carried to the model: never given again.
	void mark_supplied(const std::unordered_set<std::string> &chunkIds) {
		const std::lock_guard<std::mutex> lock(mutex_);
		supplied_.insert(chunkIds.begin(), chunkIds.end());
	}

	// How many documents this operation may know of, and how many of them it
	// can read.
	[[nodiscard]] std::size_t inventory_size() const { return inventory_.size(); }
	[[nodiscard]] std::size_t readable_count() const {
		return static_cast<std::size_t>(std::count_if(inventory_.begin(), inventory_.end(),
			[](const InventoryEntry &entry) { return entry.readable; }));
	}

	// Whether inspection can find a document this request may then propose to
	// change - one it may read, can change, and was not already offered.
	[[nodiscard]] bool may_find_changeable() const {
		if (!changeable_) return false;
		for (const InventoryEntry &entry : inventory_) {
			if (!entry.readable || nameable_.count(entry.document_id) != 0) continue;
			const matters::MatterDocument *document = document_for(entry.document_id);
			if (document != nullptr && changeable_(*document)) return true;
		}
		return false;
	}

	// One tool call: project.search, project.read or project.list. Everything it
	// returns - passages, the lines about them - fits `roomTokens` in total.
	nlohmann::json run(const std::string &toolId, const nlohmann::json &arguments, std::size_t roomTokens) {
		const std::lock_guard<std::mutex> lock(mutex_);
		if (toolId == "project.search") return search_locked(arguments.value("query", std::string()), roomTokens);
		if (toolId == "project.read") {
			std::vector<std::string> requested;
			if (arguments.contains("documents") && arguments.at("documents").is_array())
				for (const nlohmann::json &item : arguments.at("documents"))
					if (item.is_string()) requested.push_back(item.get<std::string>());
			return read_locked(requested, roomTokens);
		}
		if (toolId == "project.list") return list_locked(arguments.value("folder", std::string()), roomTokens);
		throw std::runtime_error("This Project tool is not known.");
	}

	// Every passage the operation's markers are numbered from: the request's own
	// first, then everything found, in the order they were numbered.
	[[nodiscard]] std::vector<RetrievedContextChunk> basis() const {
		const std::lock_guard<std::mutex> lock(mutex_);
		return basis_;
	}

	// The passages this session gave the model, beyond the request's own, in
	// the order they were numbered.
	[[nodiscard]] std::vector<RetrievedContextChunk> given() const {
		const std::lock_guard<std::mutex> lock(mutex_);
		std::vector<RetrievedContextChunk> result;
		for (const RetrievedContextChunk &passage : basis_)
			if (given_.count(passage.chunk.chunk_id) != 0) result.push_back(passage);
		return result;
	}

	// The documents first given here that the model was told it may also change:
	// what a model that found the document to change names as its target.
	[[nodiscard]] std::vector<std::string> newly_changeable_document_ids() const {
		const std::lock_guard<std::mutex> lock(mutex_);
		return announced_;
	}

	[[nodiscard]] std::vector<EvidenceStep> steps() const {
		const std::lock_guard<std::mutex> lock(mutex_);
		return steps_;
	}

	// What this session found and gave, by passage identity and words, in the
	// order the passages were numbered - never their text. An attempt resumed
	// after Singularity closed while it waited on the lawyer (ResearchPause.hpp)
	// gives it to restore(), so every marker the model already read means the
	// same passage again.
	[[nodiscard]] nlohmann::json kept() const {
		const std::lock_guard<std::mutex> lock(mutex_);
		nlohmann::json request = nlohmann::json::array();
		nlohmann::json found = nlohmann::json::array();
		for (std::size_t index = 0; index < basis_.size(); ++index) {
			const RetrievedContextChunk &passage = basis_[index];
			const std::string textHash = detail::passage_text_hash(passage.chunk.text);
			if (index < requestBasisSize_) {
				request.push_back(textHash);
				continue;
			}
			found.push_back({{"documentId", passage.document_id}, {"chunkId", passage.chunk.chunk_id},
				{"textHash", textHash}, {"given", given_.count(passage.chunk.chunk_id) != 0}});
		}
		return nlohmann::json{{"request", request}, {"found", found}, {"announced", announced_}};
	}

	// Takes up what an earlier attempt of the same operation found and gave
	// (kept()): each passage is read again from its document as it is now and
	// numbered where it was. True when every one is still there word for word,
	// and the request's own passages are the ones the markers were numbered
	// from; false - and nothing taken - otherwise, since a marker the model read
	// could then name another passage. Only a session that has given nothing
	// yet can take it up.
	bool restore(const nlohmann::json &kept) {
		const std::lock_guard<std::mutex> lock(mutex_);
		if (!kept.is_object() || basis_.size() != requestBasisSize_ || !given_.empty()) return false;
		const nlohmann::json request = kept.value("request", nlohmann::json::array());
		if (!request.is_array() || request.size() != requestBasisSize_) return false;
		for (std::size_t index = 0; index < requestBasisSize_; ++index)
			if (!request.at(index).is_string()
					|| request.at(index).get<std::string>() != detail::passage_text_hash(basis_[index].chunk.text))
				return false;
		std::map<std::string, std::vector<RetrievedContextChunk>> read;
		std::unordered_set<std::string> taken(known_.begin(), known_.end());
		std::vector<RetrievedContextChunk> found;
		std::unordered_set<std::string> given;
		for (const nlohmann::json &entry : kept.value("found", nlohmann::json::array())) {
			if (!entry.is_object()) return false;
			const std::string documentId = entry.value("documentId", std::string());
			const std::string chunkId = entry.value("chunkId", std::string());
			const std::string textHash = entry.value("textHash", std::string());
			if (documentId.empty() || !in_scope(documentId) || entry_for(documentId) == nullptr) return false;
			if (read.count(documentId) == 0) {
				try {
					read[documentId] = matters::retrieve_document_context(workspacePath_, project_, std::string(),
						{documentId}, nullptr, matters::SelectedSourceDelivery::EveryPassage);
				} catch (const std::exception &) {
					return false;
				}
			}
			const std::vector<RetrievedContextChunk> &passages = read.at(documentId);
			auto match = std::find_if(passages.begin(), passages.end(), [&](const RetrievedContextChunk &passage) {
				return passage.document_id == documentId && passage.chunk.chunk_id == chunkId
					&& taken.count(passage.chunk.chunk_id) == 0;
			});
			// The document was saved again: its passages have new identities, and
			// the one with the same words is the passage the model read.
			if (match == passages.end())
				match = std::find_if(passages.begin(), passages.end(), [&](const RetrievedContextChunk &passage) {
					return passage.document_id == documentId && taken.count(passage.chunk.chunk_id) == 0
						&& detail::passage_text_hash(passage.chunk.text) == textHash;
				});
			if (match == passages.end()) return false;
			RetrievedContextChunk numbered = *match;
			numbered.explicitly_selected = false;
			taken.insert(numbered.chunk.chunk_id);
			if (entry.value("given", false)) given.insert(numbered.chunk.chunk_id);
			found.push_back(std::move(numbered));
		}
		for (RetrievedContextChunk &passage : found) {
			known_.insert(passage.chunk.chunk_id);
			basis_.push_back(std::move(passage));
		}
		for (const std::string &chunkId : given) {
			supplied_.insert(chunkId);
			given_.insert(chunkId);
		}
		for (const nlohmann::json &documentId : kept.value("announced", nlohmann::json::array())) {
			if (!documentId.is_string() || document_for(documentId.get<std::string>()) == nullptr) continue;
			if (nameable_.insert(documentId.get<std::string>()).second) announced_.push_back(documentId.get<std::string>());
		}
		return true;
	}

private:
	// What one call may still return, counted by the loop's own estimate.
	struct Room {
		std::size_t remaining = 0;
	};

	static std::string path_of(const matters::MatterDocument &document) {
		return document.relative_path.empty() ? document.name : document.relative_path;
	}

	static std::string inventory_line(const InventoryEntry &entry) {
		std::string line = entry.handle + "  " + entry.path;
		if (!entry.readable) line += "  (cannot be read: " + entry.unreadable_reason + ")";
		return line + "\n";
	}

	bool in_scope(const std::string &documentId) const {
		return scope_.empty() || std::find(scope_.begin(), scope_.end(), documentId) != scope_.end();
	}

	const InventoryEntry *entry_for(const std::string &documentId) const {
		for (const InventoryEntry &entry : inventory_)
			if (entry.document_id == documentId) return &entry;
		return nullptr;
	}

	const matters::MatterDocument *document_for(const std::string &documentId) const {
		for (const matters::MatterDocument &document : project_.documents)
			if (document.id == documentId) return &document;
		return nullptr;
	}

	// A document the model names by its handle, its path or its file name -
	// only ever one of the inventory's.
	const InventoryEntry *resolve(const std::string &named) const {
		const std::string wanted = detail::lowered(detail::trimmed(named));
		if (wanted.empty()) return nullptr;
		for (const InventoryEntry &entry : inventory_)
			if (detail::lowered(entry.handle) == wanted || detail::lowered(entry.path) == wanted) return &entry;
		const InventoryEntry *byName = nullptr;
		std::size_t carriers = 0;
		for (const InventoryEntry &entry : inventory_) {
			if (detail::lowered(entry.name) != wanted) continue;
			byName = &entry;
			++carriers;
		}
		// A file name two documents share names neither.
		return carriers == 1 ? byName : nullptr;
	}

	std::size_t cost_of(const std::string &text) const {
		return estimate_ ? estimate_(text) : text.size();
	}

	// Appends `piece` whole when the call's room still holds it.
	bool admit(std::string &text, const std::string &piece, Room &room) const {
		const std::size_t cost = cost_of(piece);
		if (cost > room.remaining) return false;
		room.remaining -= cost;
		text += piece;
		return true;
	}

	// Numbers `passages` into the basis and gives, whole and in their order, as
	// many as the call's room still holds, each document's first passage under a
	// line saying which document it is and how it is cited.
	void give_locked(std::vector<RetrievedContextChunk> passages, std::string &text, Room &room, EvidenceStep &step,
			std::vector<std::pair<std::string, std::string>> &newDocuments) {
		std::unordered_set<std::string> documentsBefore;
		for (const RetrievedContextChunk &passage : basis_) documentsBefore.insert(passage.document_id);
		for (const RetrievedContextChunk &passage : passages) {
			if (!known_.insert(passage.chunk.chunk_id).second) continue;
			RetrievedContextChunk numbered = passage;
			numbered.explicitly_selected = false;
			basis_.push_back(std::move(numbered));
		}
		const SourceMarkerRegistry registry(basis_);
		std::string currentDocument;
		for (const RetrievedContextChunk &passage : passages) {
			const InventoryEntry *entry = entry_for(passage.document_id);
			const std::string name = entry != nullptr ? entry->path : passage.document_name;
			const std::string token = registry.document_token_for(passage.chunk.chunk_id);
			std::string block;
			if (passage.document_id != currentDocument)
				block += "DOCUMENT " + (entry != nullptr ? entry->handle + " " : std::string()) + name
					+ " (cited as " + token + ")\n";
			block += registry.format_chunk(passage);
			if (!admit(text, block, room)) {
				++step.passages_left_out;
				continue;
			}
			currentDocument = passage.document_id;
			supplied_.insert(passage.chunk.chunk_id);
			given_.insert(passage.chunk.chunk_id);
			++step.passages;
			if (std::find(step.document_ids.begin(), step.document_ids.end(), passage.document_id) != step.document_ids.end())
				continue;
			step.document_ids.push_back(passage.document_id);
			step.document_names.push_back(name);
			if (documentsBefore.count(passage.document_id) == 0) newDocuments.emplace_back(passage.document_id, token);
		}
	}

	// A document this request could not name before it was found here is one
	// the model may PROPOSE as the document to change - said once, with its
	// handle, when the room holds the sentence; a document the model was not
	// told of stays unnamed. Proposing grants nothing: Singularity validates the
	// named document, and the Project's action mode - or the lawyer's own
	// "show me first" - decides whether it waits for approval (artifact.action.
	// apply's always_asks).
	void announce_changeable(const std::vector<std::pair<std::string, std::string>> &newDocuments, std::string &text,
			Room &room) {
		if (!changeable_) return;
		for (const auto &[documentId, token] : newDocuments) {
			const matters::MatterDocument *document = document_for(documentId);
			if (document == nullptr || nameable_.count(documentId) != 0 || !changeable_(*document)) continue;
			const InventoryEntry *entry = entry_for(documentId);
			const std::string line = "If the request asks for " + (entry != nullptr ? entry->path : document->name)
				+ " itself to change, you may propose it as the document to change by naming " + token
				+ " in targetDocument; Singularity checks it, and the Project's permission decides whether the lawyer "
				"approves it before it is written.\n";
			if (!admit(text, line, room)) continue;
			nameable_.insert(documentId);
			announced_.push_back(documentId);
		}
	}

	nlohmann::json finished(EvidenceStep step, std::string text) {
		step.completed_at = workspaces::current_timestamp();
		nlohmann::json value{{"text", std::move(text)}, {"kind", step.kind}, {"subject", step.subject},
			{"passages", step.passages}, {"passagesLeftOut", step.passages_left_out},
			{"documents", step.document_names}, {"outcome", step.outcome}};
		steps_.push_back(std::move(step));
		return value;
	}

	nlohmann::json search_locked(const std::string &query, std::size_t roomTokens) {
		EvidenceStep step;
		step.kind = "search";
		step.subject = detail::trimmed(query);
		step.started_at = workspaces::current_timestamp();
		Room room{roomTokens};
		std::string text;
		std::vector<RetrievedContextChunk> found;
		try {
			found = matters::retrieve_document_context(workspacePath_, project_, step.subject, scope_, nullptr,
				scope_.empty() ? matters::SelectedSourceDelivery::EveryPassage
					: matters::SelectedSourceDelivery::RelevanceWithin);
		} catch (const std::exception &) {
			step.outcome = "not_readable";
			admit(text, "The Project could not be searched just now.\n", room);
			return finished(std::move(step), std::move(text));
		}
		std::vector<RetrievedContextChunk> fresh;
		std::size_t alreadyGiven = 0;
		for (RetrievedContextChunk &passage : found) {
			// Only what the words point at: a chosen document's best passage is
			// not a search result when none of the words is in it.
			if (passage.lexical_score <= 0.0) continue;
			if (supplied_.count(passage.chunk.chunk_id) != 0) {
				++alreadyGiven;
				continue;
			}
			if (fresh.size() == kMaximumSearchPassages) break;
			fresh.push_back(std::move(passage));
		}
		if (fresh.empty()) {
			step.outcome = alreadyGiven > 0 ? "already_given" : "nothing_found";
			admit(text, alreadyGiven > 0
				? "Every passage these words find was already given to you in this task.\n"
				: "No passage of the Project matches these words. Try other words, or read a document by its handle.\n",
				room);
			return finished(std::move(step), std::move(text));
		}
		std::vector<std::pair<std::string, std::string>> newDocuments;
		if (admit(text, "Passages matching \"" + step.subject + "\", best first:\n", room))
			give_locked(std::move(fresh), text, room, step, newDocuments);
		else step.passages_left_out = fresh.size();
		if (step.passages == 0) {
			step.outcome = "no_room";
			text.clear();
			return finished(std::move(step), std::move(text));
		}
		step.outcome = "given";
		if (step.passages_left_out > 0)
			admit(text, std::to_string(step.passages_left_out) + " more matching passages did not fit.\n", room);
		announce_changeable(newDocuments, text, room);
		return finished(std::move(step), std::move(text));
	}

	nlohmann::json read_locked(const std::vector<std::string> &requested, std::size_t roomTokens) {
		EvidenceStep step;
		step.kind = "read";
		step.started_at = workspaces::current_timestamp();
		// ONE room for the whole reading, however many documents it names.
		Room room{roomTokens};
		std::string text;
		std::vector<std::pair<std::string, std::string>> newDocuments;
		std::vector<std::string> named;
		std::size_t documentsRead = 0;
		for (const std::string &request : requested) {
			if (documentsRead == kMaximumDocumentsPerRead) break;
			const InventoryEntry *entry = resolve(request);
			// Unknown, or outside the lawyer's choice: the same answer, so the
			// one cannot be told from the other.
			if (entry == nullptr) {
				admit(text, "\"" + detail::trimmed(request) + "\" is not a document of this Project.\n", room);
				if (step.outcome.empty()) step.outcome = "not_found";
				continue;
			}
			named.push_back(entry->path);
			if (!entry->readable) {
				admit(text, entry->path + " cannot be read: " + entry->unreadable_reason + ".\n", room);
				if (step.outcome.empty()) step.outcome = "not_readable";
				continue;
			}
			++documentsRead;
			std::vector<RetrievedContextChunk> passages;
			try {
				passages = matters::retrieve_document_context(workspacePath_, project_, std::string(), {entry->document_id},
					nullptr, matters::SelectedSourceDelivery::EveryPassage);
			} catch (const std::exception &) {
				admit(text, entry->path + " could not be read just now.\n", room);
				if (step.outcome.empty()) step.outcome = "not_readable";
				continue;
			}
			passages.erase(std::remove_if(passages.begin(), passages.end(), [&](const RetrievedContextChunk &passage) {
				return passage.document_id != entry->document_id;
			}), passages.end());
			// A document is read the way it is written: from its beginning.
			std::stable_sort(passages.begin(), passages.end(),
				[](const RetrievedContextChunk &left, const RetrievedContextChunk &right) {
					return left.chunk.source_start < right.chunk.source_start;
				});
			const std::size_t total = passages.size();
			std::vector<RetrievedContextChunk> unread;
			for (RetrievedContextChunk &passage : passages)
				if (supplied_.count(passage.chunk.chunk_id) == 0) unread.push_back(std::move(passage));
			if (unread.empty()) {
				admit(text, entry->path + ": every passage of it was already given to you in this task.\n", room);
				if (step.outcome.empty()) step.outcome = "already_given";
				continue;
			}
			const std::size_t leftBefore = step.passages_left_out;
			const std::size_t givenBefore = step.passages;
			const std::size_t alreadyHeld = total - unread.size();
			give_locked(std::move(unread), text, room, step, newDocuments);
			if (step.passages_left_out > leftBefore)
				admit(text, entry->path + ": " + std::to_string(step.passages - givenBefore + alreadyHeld) + " of its "
					+ std::to_string(total) + " passages are now in your context; the rest did not fit.\n", room);
		}
		for (const std::string &name : named) step.subject += (step.subject.empty() ? "" : ", ") + name;
		if (step.passages > 0) step.outcome = "given";
		else if (step.passages_left_out > 0) step.outcome = "no_room";
		else if (step.outcome.empty()) step.outcome = "nothing_found";
		announce_changeable(newDocuments, text, room);
		return finished(std::move(step), std::move(text));
	}

	nlohmann::json list_locked(const std::string &folder, std::size_t roomTokens) {
		EvidenceStep step;
		step.kind = "list";
		step.subject = detail::trimmed(folder);
		while (!step.subject.empty() && step.subject.back() == '/') step.subject.pop_back();
		step.started_at = workspaces::current_timestamp();
		step.outcome = "listed";
		Room room{roomTokens};
		const std::string prefix = step.subject.empty() ? std::string() : detail::lowered(step.subject) + "/";
		std::string text;
		std::size_t shown = 0;
		std::size_t matching = 0;
		for (const InventoryEntry &entry : inventory_) {
			if (!prefix.empty() && detail::lowered(entry.path).rfind(prefix, 0) != 0) continue;
			++matching;
			if (admit(text, inventory_line(entry), room)) ++shown;
		}
		if (matching == 0) admit(text, "No document of this Project is in the folder \"" + step.subject + "\".\n", room);
		else if (shown < matching)
			admit(text, std::to_string(matching - shown) + " more documents did not fit; list a folder to see them.\n", room);
		return finished(std::move(step), std::move(text));
	}

	std::filesystem::path workspacePath_;
	matters::Matter project_;
	std::vector<std::string> scope_;
	std::vector<RetrievedContextChunk> basis_;
	// How many passages of the basis the request itself carried: everything
	// after them was found here.
	std::size_t requestBasisSize_ = 0;
	TokenEstimate estimate_;
	Changeable changeable_;
	std::unordered_set<std::string> nameable_;
	std::vector<std::string> announced_;
	std::vector<InventoryEntry> inventory_;
	std::unordered_set<std::string> known_;
	std::unordered_set<std::string> supplied_;
	std::unordered_set<std::string> given_;
	std::vector<EvidenceStep> steps_;
	mutable std::mutex mutex_;
};

// The model's own request for more of the Project, as the professional response
// carries it ({"search": ..., "read": [...], "list": ...}), as the tool calls
// that answer it: [{"toolId": ..., "arguments": {...}}], at most one of each
// kind. Empty when the request asks for nothing.
inline nlohmann::json tool_calls_for(const nlohmann::json &request) {
	nlohmann::json calls = nlohmann::json::array();
	if (!request.is_object()) return calls;
	if (request.contains("list") && request.at("list").is_string())
		calls.push_back({{"toolId", "project.list"},
			{"arguments", {{"folder", detail::trimmed(request.at("list").get<std::string>())}}}});
	if (request.contains("search") && request.at("search").is_string()) {
		std::string query = detail::trimmed(request.at("search").get<std::string>());
		if (query.size() > kMaximumSearchBytes) {
			std::size_t cut = kMaximumSearchBytes;
			while (cut > 0 && (static_cast<unsigned char>(query[cut]) & 0xC0U) == 0x80U) --cut;
			query.resize(cut);
		}
		if (!query.empty()) calls.push_back({{"toolId", "project.search"}, {"arguments", {{"query", query}}}});
	}
	if (request.contains("read") && request.at("read").is_array()) {
		nlohmann::json documents = nlohmann::json::array();
		for (const nlohmann::json &item : request.at("read"))
			if (item.is_string() && !detail::trimmed(item.get<std::string>()).empty()
					&& documents.size() < kMaximumDocumentsPerRead)
				documents.push_back(detail::trimmed(item.get<std::string>()));
		if (!documents.empty()) calls.push_back({{"toolId", "project.read"}, {"arguments", {{"documents", documents}}}});
	}
	return calls;
}

namespace detail {

inline std::mutex &registration_mutex() {
	static std::mutex mutex;
	return mutex;
}

inline bool &is_registered() {
	static bool registered = false;
	return registered;
}

inline std::optional<std::string> validate_arguments(const std::string &toolId, const nlohmann::json &arguments) {
	if (!arguments.is_object()) return std::string("invalid_project_evidence_request");
	if (toolId == "project.search") {
		if (!arguments.contains("query") || !arguments.at("query").is_string()) return std::string("invalid_project_search");
		const std::string query = trimmed(arguments.at("query").get<std::string>());
		if (query.empty() || query.size() > kMaximumSearchBytes) return std::string("invalid_project_search");
		return std::nullopt;
	}
	if (toolId == "project.read") {
		if (!arguments.contains("documents") || !arguments.at("documents").is_array()) return std::string("invalid_project_read");
		const nlohmann::json &documents = arguments.at("documents");
		if (documents.empty() || documents.size() > kMaximumDocumentsPerRead) return std::string("invalid_project_read");
		for (const nlohmann::json &item : documents)
			if (!item.is_string() || trimmed(item.get<std::string>()).empty()) return std::string("invalid_project_read");
		return std::nullopt;
	}
	if (toolId == "project.list") {
		if (!arguments.contains("folder") || !arguments.at("folder").is_string()) return std::string("invalid_project_list");
		return std::nullopt;
	}
	return std::string("unknown_project_tool");
}

} // namespace detail

// Registers project.search, project.read and project.list with the production
// ToolBroker. A Project read is a LocalRead - it changes nothing and sends
// nothing anywhere by itself, so no action mode asks before it - and it is
// answered only by the operation that offers it (ToolExecutionContext::
// project_evidence): a call from anywhere else finds nothing to answer it.
inline void ensure_production_project_evidence_tools() {
	const std::lock_guard<std::mutex> lock(detail::registration_mutex());
	if (detail::is_registered()) return;
	for (const char *id : {"project.search", "project.read", "project.list"}) {
		const std::string toolId = id;
		tools::ToolDefinition definition;
		definition.id = toolId;
		definition.capability = "project.read";
		definition.effect = tools::ToolEffectClass::LocalRead;
		definition.approval_summary = "Read this Project's documents";
		definition.validate = [toolId](const tools::ToolExecutionContext &, const nlohmann::json &arguments) {
			return detail::validate_arguments(toolId, arguments);
		};
		definition.execute = [toolId](const tools::ToolExecutionContext &context, const nlohmann::json &arguments) {
			if (!context.project_evidence) throw std::runtime_error("This request cannot read the Project's documents.");
			const nlohmann::json value = nlohmann::json::parse(
				context.project_evidence(toolId, arguments.dump(), context.project_evidence_room_tokens), nullptr, false);
			if (!value.is_object()) throw std::runtime_error("The Project could not be read for this request.");
			return value;
		};
		tools::production_tool_broker().register_tool(definition);
	}
	detail::is_registered() = true;
}

// Tests that clear the production broker's registrations register these again.
inline void reset_project_evidence_tools_for_testing() {
	const std::lock_guard<std::mutex> lock(detail::registration_mutex());
	detail::is_registered() = false;
}

} // namespace singularity::context::project_evidence
