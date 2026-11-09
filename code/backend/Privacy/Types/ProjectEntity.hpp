#pragma once

#include "PapinianGuard/Taxonomy/Taxonomy.hpp"

#include <optional>
#include <string>
#include <vector>

namespace singularity::privacy {

// Phase 10 (Privacy - entities and Project coverage). Real local protection
// preprocessing readiness for one Document, keyed to the exact extraction it
// was computed from (processed_fingerprint/processed_extractor_version) so a
// later content or extractor change is detected as stale rather than
// silently kept current - see ProjectPreprocessing.cpp.
//
// Deliberately has no "needs review" value of its own: whether a Document
// currently needs review depends on whether any entity it mentions is part
// of a PENDING ambiguity right now, which can change the moment a person
// resolves that ambiguity - a stored snapshot taken at preprocessing time
// would silently go stale the instant a review elsewhere resolves it. See
// EntityRegistry.cpp's document_has_pending_review(), computed fresh from
// the current entity_ambiguities every time, never cached here.
enum class DocumentProtectionState { NotReady, Ready, Unsupported };

struct DocumentProtectionStatus {
	std::string document_id;
	DocumentProtectionState state = DocumentProtectionState::NotReady;
	std::string processed_fingerprint;
	std::string processed_extractor_version;
	std::string ner_model_version;
	std::string detail;
	std::string updated_at;
};

// One local source mention of a ProjectEntity - never the sensitive text
// itself beyond what the entity's own aliases already retain. `alias` is the
// exact surface form this Document contributed, so Entity detail can show
// "which local source occurrence" without a separate span index - real spans
// are Phase 11's ephemeral, request-scoped concern, never this persisted one.
struct EntityOccurrence {
	std::string document_id;
	std::string alias;
	long long mention_count = 0;
};

struct ProjectEntity {
	std::string id;
	protected_api::papinian::EntityType category;
	std::string pseudonym;
	std::string canonical_value;
	std::vector<std::string> aliases;
	// Aliases a user explicitly confirmed belong here - a merge, a split
	// target, or a manually added/kept alias. Later automatic resolution
	// must never move or silently reassign one of these - see
	// EntityResolver.cpp/EntityCuration.cpp for the user-correction-priority
	// rule this backs.
	std::vector<std::string> locked_aliases;
	// Surface keys (EntityResolver.hpp's surface_alias_key()) of values a person
	// split off or removed from this entity: automatic resolution never
	// attaches such a value to it again.
	std::vector<std::string> rejected_aliases;
	// Normalized nearby strong-identifier values (an email/tax id/case
	// number detected close to a mention) ever recorded supporting this
	// entity - the real disambiguating signal EntityResolver.cpp uses so an
	// identical name string does not automatically imply the same entity.
	std::vector<std::string> context_keys;
	std::vector<EntityOccurrence> occurrences;
	// Set only when this entity was merged into another entity (a user "Same
	// organisation" resolution) - excluded from every normal list/detail
	// read, but kept rather than erased so its pseudonym is never reused and
	// a restart replays exactly the same merge outcome - see
	// EntityCuration.cpp.
	std::optional<std::string> merged_into_entity_id;
	std::string created_at;
	std::string updated_at;
};

enum class AmbiguityStatus { Pending, MergedSameEntity, KeptSeparate };

struct EntityAmbiguity {
	std::string id;
	protected_api::papinian::EntityType category;
	std::string entity_a_id;
	std::string entity_b_id;
	std::string evidence_alias_a;
	std::string evidence_alias_b;
	AmbiguityStatus status = AmbiguityStatus::Pending;
	std::string created_at;
	std::string resolved_at;
};

[[nodiscard]] const char *to_string(DocumentProtectionState state) noexcept;
[[nodiscard]] DocumentProtectionState document_protection_state_from_string(const std::string &value) noexcept;
[[nodiscard]] const char *to_string(AmbiguityStatus status) noexcept;
[[nodiscard]] AmbiguityStatus ambiguity_status_from_string(const std::string &value) noexcept;

// The plain-language category label a pseudonym is built from ("Company 1",
// "Person 1") - never the raw taxonomy enumerator name, which stays internal
// machinery (a raw NER label is never shown as normal UI).
[[nodiscard]] std::string display_category_name(protected_api::papinian::EntityType category);

} // namespace singularity::privacy
