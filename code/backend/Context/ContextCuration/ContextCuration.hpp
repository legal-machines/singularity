#pragma once

#include "Context/Types/ProjectContext.hpp"
#include "Matter/Types/Matter.hpp"

#include <optional>
#include <string>

namespace singularity::context {

// The only place canonical Context items and suggestions change - Phase
// 12's actual authority enforcement. Every function mutates `matter` in
// place and throws std::runtime_error with a plain-language message on an
// invalid request; the caller persists the result
// (Matter/MatterActions.hpp's own load-modify-save convention, see
// Context/ProjectContextActions).

// What happened when a Project-instructions-independent automatic process
// (future document/AI derivation) proposed one fact. `item_id` is always
// the affected canonical item once one exists (already-canonical or
// freshly created); `suggestion_id` is set only when the proposal could not
// be applied directly and is now waiting for a human decision instead.
struct DerivedValueOutcome {
	bool applied_directly = false;
	std::string item_id;
	std::optional<std::string> suggestion_id;
};

// Submits one automatically derived fact for (section, label) - the single
// authority policy for automatic Context. AI output never becomes canonical
// merely because a model produced it:
//   - the same value again (compared without case or spacing) changes nothing
//     but a derived item's document reference - it follows its own document's
//     newest reading, or moves to this document when its own source changed or
//     is gone - and fills a missing entity link (never a person's);
//   - a value a person already removed or dismissed for this slot is ignored;
//   - a new slot becomes a canonical AutomaticallyDerived item only for
//     identifying metadata a document states directly (parties, roles, people,
//     jurisdiction, transaction type, documents, definitions, dates, amounts,
//     group structure) with a document citation; any other new value (key facts,
//     conclusions, assumptions, instructions) is a Pending NewItem suggestion;
//   - a different value for such metadata from the derived item's own document,
//     or from a document when the item's own document is gone, follows that
//     document, keeping the previous value as a revision;
//   - any other different value - from another document (Conflict), or for an
//     item a person authored, confirmed or locked (Update) - is a Pending
//     suggestion targeting the item, leaving it untouched until a person decides.
// The same value as a waiting suggestion refreshes that suggestion, so the
// review queue never grows from re-running automation; a waiting suggestion for
// the value an item now holds is withdrawn.
DerivedValueOutcome submit_derived_value(matters::Matter &matter, ContextSection section, const std::string &label,
		const std::string &value, const ContextItemSource &source, const std::string &timestamp,
		const std::optional<std::string> &entity_id = std::nullopt);

// Applies a Pending suggestion's value to its canonical item - or to the item
// now holding its slot, creating one only when the slot is empty - sets the
// resulting item's authority to UserConfirmed, and records the decision's audit
// attribution (resolved_at/resolved_by_actor_id) on the suggestion itself -
// this is always an explicit human decision, so it may override whatever
// authority the target item currently holds. Every other Pending suggestion for
// the same slot is settled as Dismissed by the same decision. Returns the
// affected item's id. Throws if the suggestion does not exist or is no longer
// Pending. The Project's living memory: what an answer judged worth carrying to
// later chats (MatterRequestExecution, memoryBrief).
//
//   - a label nobody has used becomes an item at once: AutomaticallyDerived,
//     sourced to the chat result it came from, never confirmed by anyone;
//   - the same value again changes nothing;
//   - a new value for an item that itself came from a chat, unlocked, replaces
//     it and keeps the old one as a revision;
//   - a new value for anything a person wrote, confirmed or locked, or that a
//     document established, is only ever a suggestion for a person;
//   - a value a person already declined is never brought back.
// The one item the Project's memory brief is kept in: kMemoryBriefLabel
// (Context/Types/ProjectContext.hpp).
DerivedValueOutcome remember_from_chat(matters::Matter &matter, ContextSection section, const std::string &label,
		const std::string &value, const ContextItemSource &source, const std::string &timestamp);
std::string accept_suggestion(matters::Matter &matter, const std::string &suggestion_id,
		const std::string &actor_id, const std::string &timestamp);

// Marks a Pending suggestion Dismissed with its own audit attribution,
// leaving canonical Context completely untouched. Throws if the suggestion
// does not exist or is no longer Pending.
void dismiss_suggestion(matters::Matter &matter, const std::string &suggestion_id, const std::string &actor_id,
		const std::string &timestamp);

// Phase 13 (Context - editing, provenance and entities). A person directly
// creating/editing/removing a canonical item - distinct from the automatic
// derivation and suggestion-decision paths above, but governed by the same
// (section, label) slot invariant ContextRegistry.hpp documents.

// Creates a brand-new user-authored item. Throws if `label`/`value` is empty
// or a canonical item already occupies this (section, label) slot - editing
// that existing item is update_item_value's job, not a second create.
ContextItem create_item(matters::Matter &matter, ContextSection section, const std::string &label,
		const std::string &value, const std::string &actor_id, const std::string &timestamp);

// Phase 28 criterion 40: promotes one completed Chat result into canonical
// Project Context with truthful provenance - the ONLY path that may ever
// construct a ContextItemSource{ContextSourceKind::Chat, ...}. `matter_task_id`
// must resolve to a real MatterTask on THIS Matter with TaskStatus::Success;
// never a fabricated, foreign or still-running operation id. Idempotent by
// provenance, not merely by (section, label): a second explicit action naming
// the SAME matter_task_id (even under a different label/section) throws
// rather than creating a second Context item for the same Chat result - one
// explicit successful action creates exactly one item, and a client retry
// after an ambiguous response (timeout, crash) must never silently duplicate
// it. Still enforces the ordinary (section, label) slot invariant beneath
// that check for the same reasons create_item() does.
ContextItem create_item_from_chat_result(matters::Matter &matter, ContextSection section,
		const std::string &label, const std::string &value, const std::string &matter_task_id,
		const std::string &actor_id, const std::string &timestamp);

// A human correction to an existing canonical item's value. Bumps `revision`
// only when the value actually changes (ProjectContext.hpp's own documented
// invariant), and raises authority to UserAuthored so a later conflicting
// automatic derivation raises a suggestion instead of silently overwriting
// this correction - except a Locked item, which stays Locked (still directly
// editable by a person, just still protected from automatic re-derivation).
// When the value actually changes, the item's PREVIOUS value/authority is
// first captured as a durable ContextItemRevision - never simply discarded.
// Throws if the item does not exist or `value` is empty.
ContextItem update_item_value(matters::Matter &matter, const std::string &item_id, const std::string &value,
		const std::string &actor_id, const std::string &timestamp);

// Removes a canonical item outright - a correction that the fact itself was
// wrong, not just its value (e.g. an incorrect AutomaticallyDerived item).
// Throws if the item does not exist.
// The removal is remembered as a Dismissed suggestion (reason Removed), so
// automation never derives the same value for the same slot again.
void delete_item(matters::Matter &matter, const std::string &item_id, const std::string &actor_id = std::string(),
		const std::string &timestamp = std::string());

// Links/unlinks this item to a stable Project Entity Registry entry.
// link_item_entity throws if the item does not exist, the entity does not
// exist, or the entity has been merged into another one
// (Privacy/Types/ProjectEntity.hpp's merged_into_entity_id) - link the
// surviving entity instead. Never copies the entity's own data onto the
// item; only its stable id is stored.
ContextItem link_item_entity(matters::Matter &matter, const std::string &item_id, const std::string &entity_id,
		const std::string &actor_id, const std::string &timestamp);

// Throws if the item does not exist. A no-op (still updates audit fields) if
// the item was not linked to begin with.
ContextItem unlink_item_entity(
		matters::Matter &matter, const std::string &item_id, const std::string &actor_id, const std::string &timestamp);

// Phase 14 backend-owned classification and assembly policy. A person may
// change these controls on any canonical item. Lock affects automatic
// derivation only; it deliberately does not block update_item_value above.
ContextItem update_item_controls(matters::Matter &matter, const std::string &item_id, ContextItemKind kind,
		bool is_pinned, bool is_locked, bool is_excluded_from_ai, const std::string &actor_id,
		const std::string &timestamp);

// Applies the complete authored alternative selected during a collaboration
// conflict review. This remains a domain mutation: it records the previous
// current value, keeps the stable id, attributes the human decision, and can
// restore an item that the other concurrent edit deleted.
ContextItem replace_item_from_conflict(matters::Matter &matter, const ContextItem &authored_item,
		const std::string &actor_id, const std::string &timestamp);

} // namespace singularity::context
