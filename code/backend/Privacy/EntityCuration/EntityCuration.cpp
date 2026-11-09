#include "Privacy/EntityCuration/EntityCuration.hpp"

#include "Privacy/EntityRegistry/EntityRegistry.hpp"
#include "Privacy/EntityResolver/EntityResolver.hpp"

#include <algorithm>
#include <stdexcept>

namespace singularity::privacy {
namespace {

ProjectEntity &require_entity(matters::Matter &matter, const std::string &entity_id) {
	const auto found = std::find_if(matter.entities.begin(), matter.entities.end(),
			[&](const ProjectEntity &entity) { return entity.id == entity_id; });
	if (found == matter.entities.end() || found->merged_into_entity_id) {
		throw std::runtime_error("This entity no longer exists.");
	}
	return *found;
}

EntityAmbiguity &require_pending_ambiguity(matters::Matter &matter, const std::string &ambiguity_id) {
	const auto found = std::find_if(matter.entity_ambiguities.begin(), matter.entity_ambiguities.end(),
			[&](const EntityAmbiguity &ambiguity) { return ambiguity.id == ambiguity_id; });
	if (found == matter.entity_ambiguities.end()) throw std::runtime_error("This review no longer exists.");
	if (found->status != AmbiguityStatus::Pending) throw std::runtime_error("This review was already resolved.");
	return *found;
}

void addUnique(std::vector<std::string> &target, const std::string &value) {
	if (std::find(target.begin(), target.end(), value) == target.end()) target.push_back(value);
}

} // namespace

std::string resolve_ambiguity_same(
		matters::Matter &matter, const std::string &ambiguity_id, const std::string &timestamp) {
	EntityAmbiguity &ambiguity = require_pending_ambiguity(matter, ambiguity_id);
	const std::string survivor_id = ambiguity.entity_a_id;
	const std::string absorbed_id = ambiguity.entity_b_id;

	ProjectEntity &absorbed = require_entity(matter, absorbed_id);
	// Copy every field the survivor needs to gain before mutating the
	// survivor: absorbed and survivor may be the same underlying vector, and
	// require_entity() below re-finds the survivor by id rather than reusing
	// a second live reference into matter.entities alongside this one.
	const std::vector<std::string> absorbed_aliases = absorbed.aliases;
	const std::vector<std::string> absorbed_context_keys = absorbed.context_keys;
	const std::vector<EntityOccurrence> absorbed_occurrences = absorbed.occurrences;
	absorbed.merged_into_entity_id = survivor_id;
	absorbed.updated_at = timestamp;

	ProjectEntity &survivor = require_entity(matter, survivor_id);
	for (const std::string &alias : absorbed_aliases) {
		addUnique(survivor.aliases, alias);
		// A user-confirmed merge is itself the explicit correction: every
		// alias it brings across is locked so a later automatic pass never
		// moves it back off the survivor.
		addUnique(survivor.locked_aliases, alias);
	}
	for (const std::string &key : absorbed_context_keys) addUnique(survivor.context_keys, key);
	for (const EntityOccurrence &occurrence : absorbed_occurrences) {
		const auto existing = std::find_if(survivor.occurrences.begin(), survivor.occurrences.end(),
				[&](const EntityOccurrence &entry) { return entry.document_id == occurrence.document_id; });
		if (existing == survivor.occurrences.end()) {
			survivor.occurrences.push_back(occurrence);
			continue;
		}
		existing->mention_count += occurrence.mention_count;
	}
	survivor.updated_at = timestamp;

	EntityAmbiguity &ambiguity_ref = require_pending_ambiguity(matter, ambiguity_id);
	ambiguity_ref.status = AmbiguityStatus::MergedSameEntity;
	ambiguity_ref.resolved_at = timestamp;
	return survivor_id;
}

void resolve_ambiguity_separate(
		matters::Matter &matter, const std::string &ambiguity_id, const std::string &timestamp) {
	EntityAmbiguity &ambiguity = require_pending_ambiguity(matter, ambiguity_id);
	ambiguity.status = AmbiguityStatus::KeptSeparate;
	ambiguity.resolved_at = timestamp;
}

std::string split_entity_alias(
		matters::Matter &matter, const std::string &entity_id, const std::string &alias, const std::string &timestamp) {
	ProjectEntity &source = require_entity(matter, entity_id);
	const auto found = std::find(source.aliases.begin(), source.aliases.end(), alias);
	if (found == source.aliases.end()) throw std::runtime_error("This alias does not belong to this entity.");
	if (source.aliases.size() <= 1) throw std::runtime_error("An entity needs at least one remaining alias.");

	source.aliases.erase(found);
	source.locked_aliases.erase(
			std::remove(source.locked_aliases.begin(), source.locked_aliases.end(), alias), source.locked_aliases.end());
	// The split is remembered: this entity never takes the value back.
	addUnique(source.rejected_aliases, surface_alias_key(alias));
	if (source.canonical_value == alias && source.aliases.empty() == false) {
		source.canonical_value = source.aliases.front();
	}
	source.updated_at = timestamp;

	ProjectEntity created = make_new_entity(matter, source.category, alias, timestamp);
	created.locked_aliases.push_back(alias);
	matter.entities.push_back(created);
	return created.id;
}

void add_entity_alias(
		matters::Matter &matter, const std::string &entity_id, const std::string &alias, const std::string &timestamp) {
	if (alias.empty()) throw std::runtime_error("Enter an alias.");
	ProjectEntity &entity = require_entity(matter, entity_id);
	addUnique(entity.aliases, alias);
	addUnique(entity.locked_aliases, alias);
	// A person adding the value back overrides an earlier removal.
	const std::string surface = surface_alias_key(alias);
	entity.rejected_aliases.erase(std::remove(entity.rejected_aliases.begin(), entity.rejected_aliases.end(), surface),
			entity.rejected_aliases.end());
	entity.updated_at = timestamp;
}

void remove_entity_alias(
		matters::Matter &matter, const std::string &entity_id, const std::string &alias, const std::string &timestamp) {
	ProjectEntity &entity = require_entity(matter, entity_id);
	const auto found = std::find(entity.aliases.begin(), entity.aliases.end(), alias);
	if (found == entity.aliases.end()) throw std::runtime_error("This alias does not belong to this entity.");
	if (entity.aliases.size() <= 1) throw std::runtime_error("An entity needs at least one remaining alias.");
	entity.aliases.erase(found);
	entity.locked_aliases.erase(
			std::remove(entity.locked_aliases.begin(), entity.locked_aliases.end(), alias), entity.locked_aliases.end());
	// The removal is remembered: this entity never takes the value back.
	addUnique(entity.rejected_aliases, surface_alias_key(alias));
	if (entity.canonical_value == alias) entity.canonical_value = entity.aliases.front();
	entity.updated_at = timestamp;
}

} // namespace singularity::privacy
