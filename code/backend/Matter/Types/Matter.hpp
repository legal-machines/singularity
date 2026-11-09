#pragma once

#include "Activity/Types/MatterActivity.hpp"
#include "Chat/Types/Chat.hpp"
#include "Consilium/Types/ConsiliumConfiguration.hpp"
#include "Context/Types/ProjectContext.hpp"
#include "Correspondence/Types/Correspondence.hpp"
#include "Document/Types/DocumentEvent.hpp"
#include "Document/Types/MatterDocument.hpp"
#include "DocumentReview/Types/DocumentReview.hpp"
#include "Matter/Types/TimeEntry.hpp"
#include "Privacy/Types/ProjectEntity.hpp"
#include "WorkProduct/Types/ReviewTables.hpp"
#include "WorkProduct/Types/WorkProducts.hpp"

#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <string>
#include <vector>

namespace singularity::matters {

// Process-local merge baseline for portable .lm persistence. Carries the
// last-loaded snapshot and operation version a Matter was read at, so
// ProjectStore can diff against it and never against another writer's
// concurrent state. Never serialized to the Native API or Project storage.
struct ProjectStorageBaseline {
  long long version = 0;
  std::string snapshot;
  std::string project_path;
};

// Whether a Project's data can be read and changed. Legacy: the Project lives
// in the workspace manifest. Ready: its portable `.lm` data loaded. A cut-over
// Project whose folder or `.lm` cannot be reached is Unavailable; one whose
// `.lm` is unreadable or belongs to another Project is Damaged. Neither state
// can be changed or saved - Singularity never edits a stand-in for the real data.
enum class ProjectDataState { Legacy, Ready, Unavailable, Damaged };

// How a Project shows itself in the interface: a short name for the places its
// full name - usually its folder's, and as long as a description - does not
// fit, and its mark: initials or a glyph, on a ground, in a shape. Presentation
// only: nothing is derived from it and no model ever sees it. Empty values mean
// "Singularity decides" (the short name is derived, the mark is the initials in an
// outlined square).
struct ProjectAppearance {
  std::string short_name;
  std::string mark_glyph;
  std::string mark_text;
  std::string mark_ground;
  std::string mark_shape;

  [[nodiscard]] bool empty() const noexcept {
    return short_name.empty() && mark_glyph.empty() && mark_text.empty() && mark_ground.empty() &&
           mark_shape.empty();
  }
  friend bool operator==(const ProjectAppearance &left, const ProjectAppearance &right) {
    return std::tie(left.short_name, left.mark_glyph, left.mark_text, left.mark_ground, left.mark_shape) ==
           std::tie(right.short_name, right.mark_glyph, right.mark_text, right.mark_ground, right.mark_shape);
  }
  friend bool operator!=(const ProjectAppearance &left, const ProjectAppearance &right) { return !(left == right); }
};

// What the Project's memory keeper took from one document, and from which
// content of it: a few lines a lawyer on the matter must remember. Written by a
// local model (Context/MemoryKeeper) and kept so a document is read once - it
// is read again only when its fingerprint says its content changed.
struct MemoryDocumentDigest {
  std::string document_id;
  std::string fingerprint;
  std::string digest;
  std::string read_at;

  friend bool operator==(const MemoryDocumentDigest &left, const MemoryDocumentDigest &right) {
    return std::tie(left.document_id, left.fingerprint, left.digest, left.read_at) ==
           std::tie(right.document_id, right.fingerprint, right.digest, right.read_at);
  }
  friend bool operator!=(const MemoryDocumentDigest &left, const MemoryDocumentDigest &right) {
    return !(left == right);
  }
};

struct Matter {
  std::string id;
  std::string name;
  ProjectAppearance appearance;
  std::vector<MemoryDocumentDigest> memory_document_digests;
  std::optional<std::string> description;
  std::optional<std::string> instructions;
  std::optional<std::string> directory_path;
  std::string created_at;
  std::string updated_at;
  // Archived: when, and by whom; empty while it is open. It stands apart
  // from the firm's open Matters until a member restores it, and keeps
  // everything it holds. Carried with the Matter to every computer of it.
  std::string archived_at;
  std::string archived_by_actor_id;
  std::vector<MatterDocument> documents;
  std::vector<MatterNote> notes;
  std::vector<Issue> issues;
	// Phase 16 Work Product extends the existing specialized Note/Issue
	// foundation. Generic retained objects and Project-owned type definitions
	// live beside those objects in the same Project aggregate and are persisted
	// by the same ProjectStore operation stream.
	std::vector<WorkProductType> work_product_types;
	std::vector<WorkProduct> work_products;
	std::vector<WorkProductRevision> work_product_revisions;
	// Phase 19 reusable Consilium teams/plans are Project-owned durable
	// objects. They use ProjectStore's ordinary per-object collaboration
	// stream; execution/runs/results deliberately remain outside this model.
	std::vector<consilium::Configuration> consilium_configurations;
	// Review Tables (WorkProduct/Types/ReviewTables.hpp): each table, and one
	// record per document row it answered.
	std::vector<ReviewTable> review_tables;
	std::vector<ReviewRow> review_rows;
	// Document reviews an AI colleague carried out in a thread, each with its
	// versions and the lawyer's decisions (DocumentReview/Types).
	std::vector<document_review::DocumentReview> document_reviews;
	// The assignments colleagues carried out in threads, each with its steps
	// and its email.
	std::vector<document_review::AssignmentRecord> assignment_records;
	// The letters filed to the Matter, incoming and outgoing, each with how it
	// was filed.
	std::vector<correspondence::Letter> correspondence;
	// Its correspondence intake: the rules that file a letter here by the
	// account it reached and the mailbox it came from.
	std::vector<correspondence::IntakeRule> correspondence_intake;
	// The time its members recorded on it, each entry whose it is.
	std::vector<TimeEntry> time_entries;
  std::vector<MatterChat> chats;
  std::vector<MatterTask> tasks;
  // Explicit Project-level AI Setup differences from the Node default - see
  // AiSetupRoleOverride's own comment.
  std::vector<AiSetupRoleOverride> ai_setup_overrides;
  // Phase 10 (Privacy - entities and Project coverage) - the persistent
  // Project Entity Registry and its own local protection preprocessing
  // state.
  std::vector<privacy::ProjectEntity> entities;
  std::vector<privacy::EntityAmbiguity> entity_ambiguities;
  std::vector<privacy::DocumentProtectionStatus> document_protection;
  // Phase 12 (Context - canonical data model and authority). Canonical
  // Project Context items and their distinct suggestion queue - two
  // separate collections, never one collection with a "pending" flag (see
  // Context/Types/ProjectContext.hpp's own comment on why canonical state
  // and suggestions are distinct backend states).
  std::vector<context::ContextItem> context_items;
  std::vector<context::ContextSuggestion> context_suggestions;
  // Phase 13 (Context - editing, provenance and entities) - the durable
  // revision history a direct human edit leaves behind (see ContextItemRevision's
  // own comment). Append-only, never mutated/deleted.
  std::vector<context::ContextItemRevision> context_item_revisions;
  // The data room's journal (Document/Types/DocumentEvent.hpp): what happened
  // to the Project's documents, oldest first, appended by the document registry.
  std::vector<documents::DocumentEvent> document_events;
  // Papinian Cycle 4 (DESIGN4 §4.5, §8.3): the observation budget the
  // protected computation protocol has spent on this matter's hidden facts,
  // for the matter's whole life, and the fact anchors of files Singularity wrote,
  // by the SHA-256 of each file's extracted text. Counts, keys and offsets
  // only; empty for every Project written before them.
  PersistedObservationLedger observation_ledger;
  std::map<std::string, WrittenFileAnchors> written_file_anchors;
  std::shared_ptr<ProjectStorageBaseline> storage_baseline;
  // Process-local, never part of the Project's content: how this object's
  // data was loaded, why it is not Ready, and whether it is a cut-over
  // locator (the workspace manifest then records only where the Project
  // lives, marked as such).
  ProjectDataState data_state = ProjectDataState::Legacy;
  std::string data_state_reason;
  bool storage_cutover = false;

	// Every real physical folder's Project-relative path (Phase 08), recomputed
	// fresh on each Document/DocumentRegistry.hpp reconcile() -
	// matter::list_documents() is the only writer. Deliberately NOT a
	// persisted/versioned collection like `documents`: a folder is navigational
	// structure, not an object with its own identity/provenance, so the
	// smallest truthful model is a transient read-model projection over the
	// live filesystem, exactly like storage_baseline above - never serialized
	// to `.lm`/matters.json (see matter_to_json()/matter_from_json() in
	// MatterRepository.cpp, which do not mention this field) and never
	// diffed/patched by ProjectStore's collaborative save. The Native API
	// bridge layer (Bridge/MatterBridge/MatterBridge.cpp) serializes it
	// directly from the in-memory value a reconcile just produced.
	std::vector<std::string> folder_paths;
};

} // namespace singularity::matters
