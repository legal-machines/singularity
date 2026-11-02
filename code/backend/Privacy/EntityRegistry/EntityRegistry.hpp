#pragma once

#include "Matter/Types/Matter.hpp"
#include "Privacy/Types/ProjectEntity.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace singularity::privacy {

// Real AI-accessible Document coverage (PRIVACY_CONTRACT.md /
// phase-10-privacy-entities-coverage.md's "94% / 45 of 48 AI-accessible
// documents"). `total` is every Document already AI-ready (Document/Types/
// MatterDocument.hpp's document_lifecycle_state() == Ready) - Documents still
// Processing/Unsupported/NeedsAttention were never AI-accessible in the first
// place and are correctly excluded from both numerator and denominator, not
// counted against protection. `ready` is the subset of those whose own
// protection preprocessing is current (see is_protection_current() below).
// This is a processing-completeness ratio, never anonymization accuracy -
// callers must not relabel it.
struct ProjectCoverage {
	long long ready = 0;
	long long total = 0;
};

[[nodiscard]] ProjectCoverage compute_coverage(const matters::Matter &matter);

// One Document a person can actually act on from Privacy - either its own
// Document-domain state blocks protection outright (extraction not Ready:
// `reason` reuses that Document's own truthful attention_detail/state text,
// never invented Privacy-specific wording), or extraction is Ready but this
// Phase's own protection preprocessing has not yet reached Ready for it.
struct AttentionDocument {
	std::string document_id;
	std::string name;
	std::string reason;
};

[[nodiscard]] std::vector<AttentionDocument> list_attention_documents(const matters::Matter &matter);

// The current (non-merged-away) DocumentProtectionStatus for `document_id`,
// if this Project has recorded one yet.
[[nodiscard]] std::optional<DocumentProtectionStatus> find_document_protection(
		const matters::Matter &matter, const std::string &document_id);

// True when a previously recorded DocumentProtectionStatus still describes
// `document`'s current real content/extractor - false when either changed,
// meaning ProjectPreprocessing.cpp must re-run before this Document can count
// toward coverage again (a changed Document invalidates its protection
// state, it is never left silently marked Ready against stale content).
[[nodiscard]] bool is_protection_current(
		const DocumentProtectionStatus &status, const matters::MatterDocument &document);

// True when `document_id` has a recorded occurrence under either side of a
// currently PENDING EntityAmbiguity - computed fresh from matter.entity_
// ambiguities/entities every call, never a stored/cached flag, so resolving
// the ambiguity elsewhere (EntityCuration.cpp) is immediately reflected here
// without needing to touch this Document's own DocumentProtectionStatus.
[[nodiscard]] bool document_has_pending_review(const matters::Matter &matter, const std::string &document_id);

// Every entity in `matter` that has not been merged into another one (see
// ProjectEntity::merged_into_entity_id) - the normal Entity Registry list/
// count view. Order is stable (creation order) so a pseudonym's numbering is
// always readable top to bottom.
[[nodiscard]] std::vector<ProjectEntity> list_active_entities(const matters::Matter &matter);

[[nodiscard]] std::optional<ProjectEntity> find_entity(const matters::Matter &matter, const std::string &entity_id);

// Pending ambiguities that name `entity_id` on either side - what Entity
// detail's "needs review" section reads.
[[nodiscard]] std::vector<EntityAmbiguity> pending_ambiguities_for(
		const matters::Matter &matter, const std::string &entity_id);

[[nodiscard]] long long count_pending_ambiguities(const matters::Matter &matter);

// The next stable ordinal for `category`'s pseudonym ("Company 1", "Company
// 2", ...): one past the highest ordinal ANY entity of this category has
// ever been assigned in this Project, active or merged-away. Merged-away
// entities are deliberately still scanned so a retired pseudonym is never
// handed to a new, unrelated entity later - see ProjectEntity::merged_into_
// entity_id's own comment.
[[nodiscard]] long long next_category_ordinal(const matters::Matter &matter,
		protected_api::papinian::EntityType category);

// Builds a brand-new ProjectEntity for `category`/`canonical_value` with a
// freshly assigned stable pseudonym - the one place a pseudonym is ever
// created, so EntityResolver.cpp and EntityCuration.cpp (split) both produce
// identical, correctly-numbered identities.
[[nodiscard]] ProjectEntity make_new_entity(const matters::Matter &matter,
		protected_api::papinian::EntityType category, const std::string &canonical_value,
		const std::string &timestamp);

// Stable random ids for a new EntityAmbiguity - the collection's own id
// space, distinct from ProjectEntity's (make_new_entity() generates its own
// directly). Exposed so EntityResolver.cpp/EntityCuration.cpp never
// hand-derive an id from an unrelated object's own id.
[[nodiscard]] std::string generate_ambiguity_id();

// Records one local mention of `entity_id` from `document_id`/`alias` -
// creates a new EntityOccurrence or increments an existing one's
// mention_count. Mutates `matter.entities` in place; the caller is
// responsible for persisting it (see Matter/MatterActions.hpp's own
// load-modify-save convention).
void record_occurrence(matters::Matter &matter, const std::string &entity_id,
		const std::string &document_id, const std::string &alias);

// Every local value that stands for `entity` when a request is protected: its
// aliases, its canonical value and - for a person whose surname no other person
// in the Project shares - that bare surname. A lawyer asks about "Holt"; the
// documents say "Brennan Holt" and "Mr. Holt". With the surname among the
// entity's values the question and the documents carry one symbol for one
// party, and the bare surname never leaves the device even where the local
// model misses it. Derived on every request, never stored.
[[nodiscard]] std::vector<std::string> protection_values(const matters::Matter &matter, const ProjectEntity &entity);

// The symbol each identity of the Project stands under in every request and
// every document of it (the owner, 2026-11-02: "one map for the whole
// project, for prompt messages and documents alike"; D-235): "[[Org3]]" is
// the same organisation wherever it stands. Numbered per kind of symbol in the
// order the identities came, merged ones included so a number is never given
// again. Derived from the registry on every request, never stored; what an
// AI provider reads is still only the symbol.
[[nodiscard]] std::map<std::string, std::string> project_entity_symbols(const matters::Matter &matter);

} // namespace singularity::privacy
