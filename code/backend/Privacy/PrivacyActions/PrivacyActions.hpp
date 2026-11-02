#pragma once

#include "Matter/Types/Matter.hpp"
#include "Privacy/EntityRegistry/EntityRegistry.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace singularity::privacy_actions {

// Runs Phase 10's own local protection preprocessing pass over `id`'s
// pending Documents (Privacy/ProjectPreprocessing/ProjectPreprocessing.hpp).
// Local detection runs with no lock held; each document's result is then
// saved on its own under the PersistenceLock, against the Project as it
// stands, so a long pass never blocks other reads and coverage grows
// document by document. At most one pass runs per Project at a time. A missing/unready Papinian
// model degrades this to a no-op (a missing protection model never blocks
// Matters or local work) rather than throwing - callers needing to
// distinguish that from "ran cleanly" should inspect get_overview() instead.
matters::Matter run_preprocessing(const std::string &id);

// Whether a protection pass over `id` is running right now - one that a
// run_preprocessing() called meanwhile returns at once for, without scoring
// anything itself.
[[nodiscard]] bool preprocessing_running(const std::string &id);

// Queues run_preprocessing() for `id` on the one background protection
// worker and returns at once. Reads that notice new or changed documents call
// this, never run_preprocessing() itself: scoring a data room takes minutes.
// Launch queues every Project in turn; a Project a person is working in asks
// with `in_use`, which puts its pass ahead of the other Projects still
// waiting (never ahead of the pass already running).
void request_preprocessing(const std::string &id, bool in_use = false);

// Attaches this workspace's durable SemanticDetectionStore for the loaded
// Papinian model so protect() can reuse detections warmed when documents
// became AI-ready. No-op while the model is loading or unavailable. Safe on
// every send; does not run NER itself.
void ensure_semantic_detection_store();


// Stops the background protection worker: queued passes are dropped, a
// running one ends after its current document, and this returns once the
// worker has finished. Called when the app closes; nothing is queued after.
void stop_background_protection() noexcept;

struct ProjectOverview {
	privacy::ProjectCoverage coverage;
	long long attention_document_count = 0;
	long long entity_count = 0;
	long long pending_ambiguity_count = 0;
	// "ready" | "loading" | "not_ready" - Papinian Guard's own local
	// readiness, kept deliberately separate from the coverage numbers above
	// (PRIVACY_CONTRACT.md: never one percentage for every meaning).
	// "loading" is the normal state for the first seconds after launch; this
	// read never waits for the warm-up to finish.
	std::string guard_status;
	std::string guard_message;
};

ProjectOverview get_overview(const std::string &id);

// One document of a Project as a model receives it (the owner, 2026-11-02: "a
// switch between how a person sees it and how the machine sees it, with what
// was masked and what it was masked to"): its passages as written and as this
// computer's Papinian Guard masks them, under one table, so a name is the
// same symbol in every passage. For the window to show only: the table is
// this preview's own, never a request's, and nothing of it is kept.
struct DocumentProtectionPreview {
	// Whether this computer's Papinian Guard masks what goes out (its policy is
	// not Off); not, and a passage goes out as written.
	bool is_protected = false;
	// Whether Singularity has read the document yet; unread, it has no passages.
	bool is_read = false;
	// Its name where it stands, and as the line above its passages carries it.
	std::string name;
	std::string protected_name;
	struct Passage {
		std::string text;
		std::string protected_text;
		std::optional<std::size_t> page;
		std::optional<std::string> heading;
		// Papinian Guard could not mask it, so no request would send it.
		bool is_withheld = false;
	};
	std::vector<Passage> passages;
	// Every passage the document has; more than `passages` when it is long.
	std::size_t passage_count = 0;
	// Each symbol the passages carry, its kind, and the spellings it stands for.
	struct Replacement {
		std::string placeholder;
		std::string category;
		std::vector<std::string> forms;
	};
	std::vector<Replacement> replacements;
};

DocumentProtectionPreview preview_document_protection(const std::string &id, const std::string &document_id);
std::vector<privacy::AttentionDocument> list_attention_documents(const std::string &id);

struct EntitySummary {
	privacy::ProjectEntity entity;
	// Whether a pending ambiguity currently names this entity - the Registry
	// list's own "Review identities" affordance, computed once here rather
	// than making the Bridge layer reload the Matter per entity.
	bool needs_review = false;
};

std::vector<EntitySummary> list_entities(const std::string &id);

struct EntityDetail {
	privacy::ProjectEntity entity;
	std::vector<privacy::EntityAmbiguity> pending_ambiguities;
};

EntityDetail get_entity_detail(const std::string &id, const std::string &entity_id);

// `decision` is "same" or "separate" - "Same organisation" / "Keep separate"
// (phase-10-privacy-entities-coverage.md). Returns the surviving entity id on
// "same"; empty on "separate".
std::string resolve_ambiguity(const std::string &id, const std::string &ambiguity_id, const std::string &decision);

// "Split identity" - returns the newly created entity's id.
std::string split_entity_alias(const std::string &id, const std::string &entity_id, const std::string &alias);

void add_entity_alias(const std::string &id, const std::string &entity_id, const std::string &alias);
void remove_entity_alias(const std::string &id, const std::string &entity_id, const std::string &alias);

} // namespace singularity::privacy_actions
