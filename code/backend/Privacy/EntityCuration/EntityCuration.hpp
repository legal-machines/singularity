#pragma once

#include "Matter/Types/Matter.hpp"

#include <string>

namespace singularity::privacy {

// User curation over the Project Entity Registry - the only place
// EntityAmbiguity/ProjectEntity records change outside automatic resolution
// (EntityResolver.hpp). Every function here represents an explicit human
// decision and therefore always takes priority over later automatic
// resolution - see EntityResolver.hpp's own use of
// locked_aliases/KeptSeparate to respect that. Every function mutates
// `matter` in place and throws std::runtime_error with a plain-language
// message on an invalid request; the caller persists the result (see
// Matter/MatterActions.hpp's own load-modify-save convention).

// "Same organisation" / "[ Same organisation ]" - merges the ambiguity's
// later entity (entity_b) into its earlier one (entity_a): entity_a keeps
// its own pseudonym and gains entity_b's aliases/context_keys/occurrences
// (all newly-gained aliases become locked, since this merge is itself an
// explicit user decision). entity_b is marked merged_into_entity_id rather
// than deleted, so its retired pseudonym ordinal is never reused (see
// EntityRegistry.hpp's next_category_ordinal()) and a restart replays the
// same outcome. Returns the surviving entity's id.
std::string resolve_ambiguity_same(matters::Matter &matter, const std::string &ambiguity_id, const std::string &timestamp);

// "Keep separate" - the ambiguity's two entities remain independent
// identities. Recorded so a later automatic pass never re-raises the exact
// same pair again (EntityResolver.hpp's alreadyKeptSeparate()).
void resolve_ambiguity_separate(matters::Matter &matter, const std::string &ambiguity_id, const std::string &timestamp);

// "Split identity" - removes `alias` from `entity_id` and gives it a
// brand-new entity of its own (a freshly assigned pseudonym), with `alias`
// locked to that new entity so it can never be silently reattached to the
// old one again. Historical occurrence counts already recorded against
// `entity_id` are not retroactively reassigned - see EntityCuration.cpp's own
// comment on why occurrence records are not alias-precise. Returns the new
// entity's id.
std::string split_entity_alias(
		matters::Matter &matter, const std::string &entity_id, const std::string &alias, const std::string &timestamp);

// "Edit aliases" (add) - adds `alias` to `entity_id` as a locked, user-
// confirmed alias. A no-op if the exact alias is already present.
void add_entity_alias(
		matters::Matter &matter, const std::string &entity_id, const std::string &alias, const std::string &timestamp);

// "Edit aliases" (remove) - forgets `alias` on `entity_id` entirely (never
// creates a new entity for it - see split_entity_alias() for that). Refuses
// to remove an entity's only remaining alias: an entity with zero aliases is
// not a valid registry record.
void remove_entity_alias(
		matters::Matter &matter, const std::string &entity_id, const std::string &alias, const std::string &timestamp);

} // namespace singularity::privacy
