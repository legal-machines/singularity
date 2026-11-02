#include "Context/ContextCuration/ContextCuration.hpp"

#include "Context/ContextRegistry/ContextRegistry.hpp"
#include "Privacy/EntityRegistry/EntityRegistry.hpp"

#include <algorithm>
#include <stdexcept>

namespace singularity::context {
namespace {

ContextItem &require_item(matters::Matter &matter, const std::string &item_id) {
	const auto found = std::find_if(matter.context_items.begin(), matter.context_items.end(),
			[&](const ContextItem &item) { return item.id == item_id; });
	if (found == matter.context_items.end()) throw std::runtime_error("This Context item no longer exists.");
	return *found;
}

ContextSuggestion &require_pending_suggestion(matters::Matter &matter, const std::string &suggestion_id) {
	const auto found = std::find_if(matter.context_suggestions.begin(), matter.context_suggestions.end(),
			[&](const ContextSuggestion &suggestion) { return suggestion.id == suggestion_id; });
	if (found == matter.context_suggestions.end()) throw std::runtime_error("This suggestion no longer exists.");
	if (found->status != ContextSuggestionStatus::Pending) throw std::runtime_error("This suggestion was already resolved.");
	return *found;
}

constexpr const char *kAutomaticContextActor = "singularity:automatic-context";

// Sections whose values are identifying metadata a document states directly
// (who, where, which document, which date or amount). A new value there may
// become canonical automatically; anything that interprets the Project - key
// facts, conclusions, assumptions, instructions, tasks - waits for a person.
bool automatically_canonical_section(ContextSection section) noexcept {
	switch (section) {
		case ContextSection::Parties:
		case ContextSection::Roles:
		case ContextSection::KeyPeople:
		case ContextSection::Jurisdiction:
		case ContextSection::TransactionType:
		case ContextSection::KeyDocuments:
		case ContextSection::Definitions:
		case ContextSection::Dates:
		case ContextSection::Amounts:
		case ContextSection::GroupStructure:
			return true;
		default:
			return false;
	}
}

bool from_same_source(const ContextItemSource &current, const ContextItemSource &proposed) {
	if (current.kind != proposed.kind) return false;
	if (current.kind != ContextSourceKind::Document) return current.chat_operation_id == proposed.chat_operation_id;
	return current.document_reference && proposed.document_reference
		&& current.document_reference->document_id == proposed.document_reference->document_id;
}

bool from_same_document(const std::optional<ContextItemSource> &current, const ContextItemSource &proposed) {
	return current && current->kind == ContextSourceKind::Document && from_same_source(*current, proposed);
}

// A person already removed this value, or dismissed it as a suggestion: the
// same value for the same slot is never offered again by automation.
bool declined_before(const matters::Matter &matter, ContextSection section, const std::string &label,
		const std::string &value) {
	const std::string normalizedLabel = normalized_context_text(label);
	const std::string normalizedValue = normalized_context_text(value);
	return std::any_of(matter.context_suggestions.begin(), matter.context_suggestions.end(),
			[&](const ContextSuggestion &suggestion) {
				return suggestion.status == ContextSuggestionStatus::Dismissed && suggestion.section == section
						&& normalized_context_text(suggestion.label) == normalizedLabel
						&& normalized_context_text(suggestion.value) == normalizedValue;
			});
}

bool same_slot(const ContextSuggestion &suggestion, ContextSection section, const std::string &normalized_label) {
	return suggestion.section == section && normalized_context_text(suggestion.label) == normalized_label;
}

// One Pending suggestion per slot, target and value: the same value again
// refreshes it (moving to this source when its own source changed or is gone),
// so re-running automation never grows the review queue.
std::string suggest(matters::Matter &matter, ContextSection section, const std::string &label, const std::string &value,
		const std::optional<std::string> &target_item_id, const ContextItemSource &source,
		const std::optional<std::string> &entity_id, ContextSuggestionReason reason, const std::string &timestamp) {
	const std::string normalizedLabel = normalized_context_text(label);
	const std::string normalizedValue = normalized_context_text(value);
	for (ContextSuggestion &pending : matter.context_suggestions) {
		if (pending.status != ContextSuggestionStatus::Pending || !same_slot(pending, section, normalizedLabel)
				|| pending.target_item_id != target_item_id || normalized_context_text(pending.value) != normalizedValue)
			continue;
		if (from_same_source(pending.source, source) || context_source_status(matter, pending.source) != ContextSourceStatus::Current)
			pending.source = source;
		if (!pending.entity_id) pending.entity_id = entity_id;
		pending.reason = reason;
		return pending.id;
	}
	ContextSuggestion suggestion;
	suggestion.id = generate_suggestion_id();
	suggestion.section = section;
	suggestion.label = label;
	suggestion.value = value;
	suggestion.target_item_id = target_item_id;
	suggestion.source = source;
	suggestion.entity_id = entity_id;
	suggestion.status = ContextSuggestionStatus::Pending;
	suggestion.reason = reason;
	suggestion.created_at = timestamp;
	const std::string id = suggestion.id;
	matter.context_suggestions.push_back(std::move(suggestion));
	return id;
}

// Pending suggestions proposing the value an item now holds have nothing left
// to decide; they are withdrawn (not dismissed, which would decline the value).
void withdraw_satisfied_suggestions(matters::Matter &matter, const ContextItem &item) {
	const std::string normalizedLabel = normalized_context_text(item.label);
	const std::string normalizedValue = normalized_context_text(item.value);
	matter.context_suggestions.erase(std::remove_if(matter.context_suggestions.begin(), matter.context_suggestions.end(),
			[&](const ContextSuggestion &suggestion) {
				return suggestion.status == ContextSuggestionStatus::Pending && same_slot(suggestion, item.section, normalizedLabel)
						&& normalized_context_text(suggestion.value) == normalizedValue;
			}), matter.context_suggestions.end());
}

void record_revision(matters::Matter &matter, const ContextItem &item, const std::string &actor_id,
		const std::string &timestamp) {
	ContextItemRevision revision;
	revision.id = generate_revision_id();
	revision.item_id = item.id;
	revision.value = item.value;
	revision.authority = item.authority;
	revision.actor_id = actor_id;
	revision.created_at = timestamp;
	matter.context_item_revisions.push_back(std::move(revision));
}

} // namespace

DerivedValueOutcome submit_derived_value(matters::Matter &matter, ContextSection section, const std::string &label,
		const std::string &value, const ContextItemSource &source, const std::string &timestamp,
		const std::optional<std::string> &entity_id) {
	DerivedValueOutcome outcome;
	if (label.empty() || value.empty()) return outcome;
	const bool documentSource = source.kind == ContextSourceKind::Document && source.document_reference.has_value();
	const auto existing = find_item_by_label(matter, section, label);

	if (existing && normalized_context_text(existing->value) == normalized_context_text(value)) {
		// The same value again. A derived item follows its own document's newest
		// reading, or moves to this document when its own source changed or is
		// gone; a person's entity link is never replaced.
		ContextItem &item = require_item(matter, existing->id);
		if (item.authority == ContextItemAuthority::AutomaticallyDerived && !item.is_locked && documentSource) {
			if (from_same_document(item.source, source) || !item.source
					|| context_source_status(matter, *item.source) != ContextSourceStatus::Current) {
				item.source = source;
				item.updated_at = timestamp;
			}
			if (!item.entity_id && entity_id) {
				item.entity_id = entity_id;
				item.updated_at = timestamp;
			}
		}
		withdraw_satisfied_suggestions(matter, item);
		outcome.item_id = item.id;
		outcome.applied_directly = true;
		return outcome;
	}
	if (declined_before(matter, section, label, value)) return outcome;

	if (!existing) {
		if (automatically_canonical_section(section) && documentSource) {
			ContextItem item;
			item.id = generate_item_id();
			item.section = section;
			item.label = label;
			item.value = value;
			item.authority = ContextItemAuthority::AutomaticallyDerived;
			item.kind = default_context_item_kind(section);
			item.source = source;
			item.entity_id = entity_id;
			item.revision = 1;
			item.created_at = timestamp;
			item.updated_at = timestamp;
			outcome.item_id = item.id;
			outcome.applied_directly = true;
			matter.context_items.push_back(std::move(item));
			return outcome;
		}
		outcome.suggestion_id = suggest(matter, section, label, value, std::nullopt, source, entity_id,
			ContextSuggestionReason::NewItem, timestamp);
		return outcome;
	}

	ContextItem &item = require_item(matter, existing->id);
	outcome.item_id = item.id;
	const bool derivedAndUnlocked = item.authority == ContextItemAuthority::AutomaticallyDerived && !item.is_locked;
	const bool ownSourceGone = !item.source
		|| context_source_status(matter, *item.source) == ContextSourceStatus::Unavailable;
	if (derivedAndUnlocked && automatically_canonical_section(section) && documentSource
			&& (from_same_document(item.source, source) || ownSourceGone)) {
		// The document this value came from now says something else, or that
		// document is gone and this one states the slot: the value follows the
		// document, and the previous value is kept as a revision.
		record_revision(matter, item, kAutomaticContextActor, timestamp);
		item.value = value;
		item.source = source;
		if (!item.entity_id) item.entity_id = entity_id;
		item.revision += 1;
		item.updated_at = timestamp;
		withdraw_satisfied_suggestions(matter, item);
		outcome.applied_directly = true;
		return outcome;
	}

	// A different value from another source, or for a value a person set,
	// confirmed or locked: never applied silently. Two documents disagreeing
	// about a derived value is a conflict a person resolves.
	const bool conflict = derivedAndUnlocked && !from_same_document(item.source, source);
	outcome.suggestion_id = suggest(matter, section, label, value, item.id, source, entity_id,
		conflict ? ContextSuggestionReason::Conflict : ContextSuggestionReason::Update, timestamp);
	return outcome;
}

DerivedValueOutcome remember_from_chat(matters::Matter &matter, ContextSection section, const std::string &label,
		const std::string &value, const ContextItemSource &source, const std::string &timestamp) {
	DerivedValueOutcome outcome;
	if (label.empty() || value.empty()) return outcome;
	const auto existing = find_item_by_label(matter, section, label);
	if (existing && normalized_context_text(existing->value) == normalized_context_text(value)) {
		outcome.item_id = existing->id;
		outcome.applied_directly = true;
		return outcome;
	}
	if (declined_before(matter, section, label, value)) return outcome;

	if (!existing) {
		ContextItem item;
		item.id = generate_item_id();
		item.section = section;
		item.label = label;
		item.value = value;
		item.authority = ContextItemAuthority::AutomaticallyDerived;
		item.kind = default_context_item_kind(section);
		item.source = source;
		item.revision = 1;
		item.created_at = timestamp;
		item.updated_at = timestamp;
		outcome.item_id = item.id;
		outcome.applied_directly = true;
		matter.context_items.push_back(std::move(item));
		return outcome;
	}

	ContextItem &item = require_item(matter, existing->id);
	outcome.item_id = item.id;
	// Singularity's own: a note it filed from a chat, or the memory brief itself -
	// which stays its own to rewrite whether a chat or a person last shaped it.
	const bool ownChatNote = item.authority == ContextItemAuthority::AutomaticallyDerived && !item.is_locked
		&& (item.label == kMemoryBriefLabel || (item.source && item.source->kind == ContextSourceKind::Chat));
	if (ownChatNote) {
		record_revision(matter, item, kAutomaticContextActor, timestamp);
		item.value = value;
		item.source = source;
		item.revision += 1;
		item.updated_at = timestamp;
		withdraw_satisfied_suggestions(matter, item);
		outcome.applied_directly = true;
		return outcome;
	}
	// Anything a person or a document established is theirs to change.
	outcome.suggestion_id = suggest(matter, section, label, value, item.id, source, std::nullopt,
		ContextSuggestionReason::Update, timestamp);
	return outcome;
}

std::string accept_suggestion(
		matters::Matter &matter, const std::string &suggestion_id, const std::string &actor_id, const std::string &timestamp) {
	ContextSuggestion &suggestion = require_pending_suggestion(matter, suggestion_id);
	std::string item_id;

	// The suggestion's own item, or whichever item now holds its slot (a person
	// may have created one meanwhile): one item per slot, never a duplicate.
	auto target = suggestion.target_item_id
		? std::find_if(matter.context_items.begin(), matter.context_items.end(),
			[&](const ContextItem &item) { return item.id == *suggestion.target_item_id; })
		: matter.context_items.end();
	if (target == matter.context_items.end()) {
		const std::string normalizedLabel = normalized_context_text(suggestion.label);
		target = std::find_if(matter.context_items.begin(), matter.context_items.end(), [&](const ContextItem &item) {
			return item.section == suggestion.section && normalized_context_text(item.label) == normalizedLabel;
		});
	}
	if (target != matter.context_items.end()) {
		ContextItem &item = *target;
		if (item.value != suggestion.value) {
			record_revision(matter, item, actor_id, timestamp);
			item.revision += 1;
		}
		item.value = suggestion.value;
		item.authority = ContextItemAuthority::UserConfirmed;
		item.source = suggestion.source;
		if (suggestion.entity_id) item.entity_id = suggestion.entity_id;
		item.updated_at = timestamp;
		item.updated_by_actor_id = actor_id;
		item_id = item.id;
	} else {
		// A brand-new value, or one whose item a person removed meanwhile: the
		// accepted value becomes an item of its own.
		ContextItem item;
		item.id = generate_item_id();
		item.section = suggestion.section;
		item.label = suggestion.label;
		item.value = suggestion.value;
		item.authority = ContextItemAuthority::UserConfirmed;
		item.kind = default_context_item_kind(suggestion.section);
		item.source = suggestion.source;
		item.entity_id = suggestion.entity_id;
		item.revision = 1;
		item.created_at = timestamp;
		item.updated_at = timestamp;
		item.updated_by_actor_id = actor_id;
		item_id = item.id;
		matter.context_items.push_back(std::move(item));
	}

	// The suggestion vector is untouched above, so `suggestion` is still live.
	suggestion.status = ContextSuggestionStatus::Accepted;
	suggestion.resolved_at = timestamp;
	suggestion.resolved_by_actor_id = actor_id;
	// Choosing this value settles every other value waiting for the same slot.
	const std::string normalizedLabel = normalized_context_text(suggestion.label);
	const ContextSection section = suggestion.section;
	for (ContextSuggestion &other : matter.context_suggestions) {
		if (other.id == suggestion_id || other.status != ContextSuggestionStatus::Pending) continue;
		if (!same_slot(other, section, normalizedLabel)) continue;
		other.status = ContextSuggestionStatus::Dismissed;
		other.resolved_at = timestamp;
		other.resolved_by_actor_id = actor_id;
	}
	return item_id;
}

void dismiss_suggestion(
		matters::Matter &matter, const std::string &suggestion_id, const std::string &actor_id, const std::string &timestamp) {
	ContextSuggestion &suggestion = require_pending_suggestion(matter, suggestion_id);
	suggestion.status = ContextSuggestionStatus::Dismissed;
	suggestion.resolved_at = timestamp;
	suggestion.resolved_by_actor_id = actor_id;
}

ContextItem create_item(matters::Matter &matter, ContextSection section, const std::string &label,
		const std::string &value, const std::string &actor_id, const std::string &timestamp) {
	if (label.empty()) throw std::runtime_error("A label is required.");
	if (value.empty()) throw std::runtime_error("A value is required.");
	if (find_item_by_label(matter, section, label)) {
		throw std::runtime_error("An item already exists for this label. Edit it instead.");
	}

	ContextItem item;
	item.id = generate_item_id();
	item.section = section;
	item.label = label;
	item.value = value;
	item.authority = ContextItemAuthority::UserAuthored;
	item.kind = default_context_item_kind(section);
	item.source = ContextItemSource{ContextSourceKind::User, std::nullopt, ""};
	item.revision = 1;
	item.created_at = timestamp;
	item.updated_at = timestamp;
	item.updated_by_actor_id = actor_id;
	matter.context_items.push_back(item);
	return item;
}

ContextItem create_item_from_chat_result(matters::Matter &matter, ContextSection section,
		const std::string &label, const std::string &value, const std::string &matter_task_id,
		const std::string &actor_id, const std::string &timestamp) {
	if (label.empty()) throw std::runtime_error("A label is required.");
	if (value.empty()) throw std::runtime_error("A value is required.");
	// Truthful provenance: only a real, already-successfully-completed
	// MatterTask on THIS Matter may be promoted - never a fabricated, foreign
	// (belonging to a different Matter) or still-running/failed operation id.
	const auto task = std::find_if(matter.tasks.begin(), matter.tasks.end(),
			[&](const matters::MatterTask &entry) { return entry.id == matter_task_id; });
	if (task == matter.tasks.end())
		throw std::runtime_error("The source Chat result no longer exists in this Project.");
	if (task->status != matters::TaskStatus::Success)
		throw std::runtime_error("Only a successfully completed Chat result can be added to Context.");
	// One explicit successful action creates exactly one Context item for
	// that Chat result, regardless of label/section - checked BEFORE the
	// ordinary (section, label) slot check below so a retry after an
	// ambiguous client-side response (the create actually succeeded, but the
	// caller never saw the reply) fails closed with a clear, specific reason
	// instead of either duplicating the item or masking the real cause behind
	// a generic "label already exists".
	const bool already_promoted = std::any_of(matter.context_items.begin(), matter.context_items.end(),
			[&](const ContextItem &item) {
				// What Singularity noted from this result on its own is not a person's
				// promotion of it: only an item a person added counts here.
				return item.source && item.source->kind == ContextSourceKind::Chat
						&& item.source->chat_operation_id == matter_task_id
						&& item.authority != ContextItemAuthority::AutomaticallyDerived;
			});
	if (already_promoted)
		throw std::runtime_error("This Chat result has already been added to Context.");
	if (find_item_by_label(matter, section, label))
		throw std::runtime_error("An item already exists for this label. Edit it instead.");

	ContextItem item;
	item.id = generate_item_id();
	item.section = section;
	item.label = label;
	item.value = value;
	item.authority = ContextItemAuthority::UserAuthored;
	item.kind = default_context_item_kind(section);
	item.source = ContextItemSource{ContextSourceKind::Chat, std::nullopt, matter_task_id};
	item.revision = 1;
	item.created_at = timestamp;
	item.updated_at = timestamp;
	item.updated_by_actor_id = actor_id;
	matter.context_items.push_back(item);
	return item;
}

ContextItem update_item_value(matters::Matter &matter, const std::string &item_id, const std::string &value,
		const std::string &actor_id, const std::string &timestamp) {
	if (value.empty()) throw std::runtime_error("A value is required.");
	ContextItem &item = require_item(matter, item_id);

	if (item.value != value) {
		// Capture the value/authority being replaced as a durable revision
		// BEFORE overwriting it - COLLABORATION_CONTRACT.md's "previous version
		// and resulting version", never simply discarded.
		ContextItemRevision revision;
		revision.id = generate_revision_id();
		revision.item_id = item.id;
		revision.value = item.value;
		revision.authority = item.authority;
		revision.actor_id = actor_id;
		revision.created_at = timestamp;
		matter.context_item_revisions.push_back(std::move(revision));

		item.value = value;
		item.revision += 1;
	}
	if (item.authority != ContextItemAuthority::Locked) item.authority = ContextItemAuthority::UserAuthored;
	item.updated_at = timestamp;
	item.updated_by_actor_id = actor_id;
	return item;
}

void delete_item(matters::Matter &matter, const std::string &item_id, const std::string &actor_id,
		const std::string &timestamp) {
	const auto found = std::find_if(matter.context_items.begin(), matter.context_items.end(),
			[&](const ContextItem &item) { return item.id == item_id; });
	if (found == matter.context_items.end()) throw std::runtime_error("This Context item no longer exists.");
	// Removing a value is remembered, so automation never derives the same value
	// for the same slot again.
	ContextSuggestion removed;
	removed.id = generate_suggestion_id();
	removed.section = found->section;
	removed.label = found->label;
	removed.value = found->value;
	removed.target_item_id = found->id;
	removed.source = found->source.value_or(ContextItemSource{});
	removed.entity_id = found->entity_id;
	removed.status = ContextSuggestionStatus::Dismissed;
	removed.reason = ContextSuggestionReason::Removed;
	removed.created_at = timestamp;
	removed.resolved_at = timestamp;
	if (!actor_id.empty()) removed.resolved_by_actor_id = actor_id;
	matter.context_items.erase(found);
	matter.context_suggestions.push_back(std::move(removed));
}

ContextItem link_item_entity(matters::Matter &matter, const std::string &item_id, const std::string &entity_id,
		const std::string &actor_id, const std::string &timestamp) {
	ContextItem &item = require_item(matter, item_id);
	const auto entity = privacy::find_entity(matter, entity_id);
	if (!entity) throw std::runtime_error("This entity no longer exists.");
	if (entity->merged_into_entity_id) {
		throw std::runtime_error("This entity has been merged; link the surviving entity instead.");
	}

	item.entity_id = entity_id;
	item.updated_at = timestamp;
	item.updated_by_actor_id = actor_id;
	return item;
}

ContextItem unlink_item_entity(
		matters::Matter &matter, const std::string &item_id, const std::string &actor_id, const std::string &timestamp) {
	ContextItem &item = require_item(matter, item_id);
	item.entity_id.reset();
	item.updated_at = timestamp;
	item.updated_by_actor_id = actor_id;
	return item;
}

ContextItem update_item_controls(matters::Matter &matter, const std::string &item_id, ContextItemKind kind,
		bool is_pinned, bool is_locked, bool is_excluded_from_ai, const std::string &actor_id,
		const std::string &timestamp) {
	ContextItem &item = require_item(matter, item_id);
	// `Locked` was the pre-Phase-14 authority representation. Once the new
	// independent control is touched, normalize an unlocked legacy item so
	// the old enum cannot continue blocking automatic updates invisibly.
	if (is_locked == false && item.authority == ContextItemAuthority::Locked) {
		item.authority = ContextItemAuthority::UserAuthored;
	}
	item.kind = kind;
	item.is_pinned = is_pinned;
	item.is_locked = is_locked;
	item.is_excluded_from_ai = is_excluded_from_ai;
	item.updated_at = timestamp;
	item.updated_by_actor_id = actor_id;
	return item;
}

ContextItem replace_item_from_conflict(matters::Matter &matter, const ContextItem &authored_item,
		const std::string &actor_id, const std::string &timestamp) {
	if (authored_item.id.empty() || authored_item.value.empty()) {
		throw std::runtime_error("The concurrent Context item is not valid.");
	}
	const auto found = std::find_if(matter.context_items.begin(), matter.context_items.end(),
			[&](const ContextItem &item) { return item.id == authored_item.id; });
	ContextItem replacement = authored_item;
	if (found != matter.context_items.end()) {
		ContextItemRevision revision;
		revision.id = generate_revision_id();
		revision.item_id = found->id;
		revision.value = found->value;
		revision.authority = found->authority;
		revision.actor_id = actor_id;
		revision.created_at = timestamp;
		matter.context_item_revisions.push_back(std::move(revision));
		replacement.revision = std::max(found->revision, authored_item.revision) + 1;
		replacement.created_at = found->created_at;
		*found = replacement;
	} else {
		replacement.revision = std::max<long long>(authored_item.revision, 1) + 1;
		matter.context_items.push_back(replacement);
	}

	ContextItem &resolved = require_item(matter, authored_item.id);
	resolved.updated_at = timestamp;
	resolved.updated_by_actor_id = actor_id;
	return resolved;
}

} // namespace singularity::context
