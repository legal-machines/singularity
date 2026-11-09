#pragma once

#include "Context/SourceMarkers/SourceMarkers.hpp"

#include <optional>
#include <string>
#include <vector>

namespace singularity::context {

// The one item the Project's memory brief is kept in (KeyFacts, this label).
inline constexpr const char *kMemoryBriefLabel = "Memory";

// Phase 12 (Context - canonical data model and authority). The stable
// content categories Project Context can hold - a closed, named taxonomy
// rather than a free-form string, so every canonical item/suggestion always
// has one unambiguous section. Mirrors PapinianGuard::EntityType's own
// stable-taxonomy-plus-display-name shape (see display_section_name).
enum class ContextSection {
	Parties,
	Roles,
	KeyPeople,
	Jurisdiction,
	TransactionType,
	KeyDocuments,
	Definitions,
	Dates,
	Amounts,
	GroupStructure,
	KeyFacts,
	UserTasks,
	WorkingAssumptions,
	MaterialConclusions,
	ProjectInstructions,
	PinnedInformation,
	Other,
};

[[nodiscard]] const std::vector<ContextSection> &all_context_sections();
[[nodiscard]] const char *to_string(ContextSection section) noexcept;
[[nodiscard]] ContextSection context_section_from_string(const std::string &value) noexcept;
[[nodiscard]] std::string display_section_name(ContextSection section);

// Authority over one canonical Context item's current value. Automatic
// derivation may freely update its own AutomaticallyDerived value, but never
// a UserAuthored/UserConfirmed value, and never a Locked one even when
// re-deriving the identical content - see ContextCuration.cpp's
// submit_derived_value, which is the only place this rule is enforced.
enum class ContextItemAuthority {
	AutomaticallyDerived,
	UserConfirmed,
	UserAuthored,
	Locked,
};

[[nodiscard]] const char *to_string(ContextItemAuthority authority) noexcept;
[[nodiscard]] ContextItemAuthority context_item_authority_from_string(const std::string &value) noexcept;

enum class ContextItemKind { Fact, WorkingAssumption, Instruction, Conclusion };

[[nodiscard]] const char *to_string(ContextItemKind kind) noexcept;
[[nodiscard]] ContextItemKind context_item_kind_from_string(const std::string &value) noexcept;
[[nodiscard]] ContextItemKind default_context_item_kind(ContextSection section) noexcept;

enum class ContextSourceKind { User, Document, Chat };

[[nodiscard]] const char *to_string(ContextSourceKind kind) noexcept;
[[nodiscard]] ContextSourceKind context_source_kind_from_string(const std::string &value) noexcept;

// Where one Context value currently comes from - backend-owned provenance
// (critical invariant: item/revision/source identity and authority are
// backend-owned). A Document source reuses the same ResolvedSourceReference
// citations already carry (Context/SourceMarkers/SourceMarkers.hpp), so
// Context provenance is never a second, independent reference format.
struct ContextItemSource {
	ContextSourceKind kind = ContextSourceKind::User;
	std::optional<ResolvedSourceReference> document_reference;
	std::string chat_operation_id;
};

// One canonical Project Context fact - stable id/revision/source identity.
// `revision` increments every time `value` actually changes, so later
// provenance/merge surfaces can tell whether a held reference still points
// at the current value.
struct ContextItem {
	std::string id;
	ContextSection section = ContextSection::Other;
	std::string label;
	std::string value;
	ContextItemAuthority authority = ContextItemAuthority::AutomaticallyDerived;
	std::optional<ContextItemSource> source;
	long long revision = 1;
	std::string created_at;
	std::string updated_at;
	// Set only when a person directly created or edited this item (Phase 13 -
	// Context: editing, provenance and entities).
	std::optional<std::string> updated_by_actor_id;
	// Never a second identity store: this only holds the existing entity's id,
	// resolved back through Privacy/EntityRegistry.hpp whenever the linked
	// identity itself is needed (never copied/duplicated here). Unset when this
	// item is not about a registered entity.
	std::optional<std::string> entity_id;
	// Phase 14 controls are durable backend policy, never frontend labels.
	// Pinned items are considered before ordinary Context during assembly;
	// locked items reject automatic overwrite but remain human-editable; an
	// excluded item never enters any PromptAssembler-backed AI workflow.
	ContextItemKind kind = ContextItemKind::Fact;
	bool is_pinned = false;
	bool is_locked = false;
	bool is_excluded_from_ai = false;
};

// The exact canonical Context value one AI operation actually received.
// This is embedded in that operation's immutable MatterTask provenance,
// rather than resolved back through the current Context later: an edit or
// deletion must never rewrite what a historical operation used. Only items
// admitted by PromptAssembler's real request budget are captured.
struct ContextItemSnapshot {
	std::string item_id;
	long long revision = 1;
	ContextSection section = ContextSection::Other;
	ContextItemKind kind = ContextItemKind::Fact;
	std::string label;
	std::string value;
	ContextItemAuthority authority = ContextItemAuthority::AutomaticallyDerived;
	std::optional<ContextItemSource> source;
	std::optional<std::string> entity_id;
	bool is_pinned = false;
	bool is_locked = false;
	bool is_excluded_from_ai = false;
};

struct ContextSearchResult {
	std::string item_id;
	ContextSection section = ContextSection::Other;
	std::string label;
	std::string value;
};

enum class ContextSurfaceState { Empty, Building, Partial, Conflict, Ready };

[[nodiscard]] const char *to_string(ContextSurfaceState state) noexcept;

struct ContextPageState {
	ContextSurfaceState state = ContextSurfaceState::Empty;
	long long item_count = 0;
	long long pending_suggestion_count = 0;
	long long conflict_count = 0;
	long long processed_document_count = 0;
	long long total_document_count = 0;
	bool automatic_updates_available = false;
};

// A durable snapshot of a canonical item's value immediately before a human
// edit replaced it. Append-only, never mutated or deleted - the same
// durable-record shape ContextSuggestion already uses for suggestion
// decisions (see its own comment), applied here to direct edits instead of
// suggestion resolutions. `actor_id`/`created_at` describe the edit that
// superseded `value`, not who originally authored it.
struct ContextItemRevision {
	std::string id;
	std::string item_id;
	std::string value;
	ContextItemAuthority authority;
	std::string actor_id;
	std::string created_at;
};

enum class ContextSuggestionStatus { Pending, Accepted, Dismissed };

// Why a suggestion exists: a value for a slot Context does not hold yet
// (NewItem), a different value for an item a person owns or locked (Update), a
// value from another source that disagrees with an automatically derived item's
// own source (Conflict), or the durable record that a person removed a value
// (Removed, always Dismissed) - never offered again from automation.
enum class ContextSuggestionReason { NewItem, Update, Conflict, Removed };

[[nodiscard]] const char *to_string(ContextSuggestionReason reason) noexcept;
[[nodiscard]] ContextSuggestionReason context_suggestion_reason_from_string(const std::string &value) noexcept;

// Whether a document-sourced item still matches its document: the document is
// gone or unavailable, or its content changed since the value was taken.
enum class ContextSourceStatus { Current, Changed, Unavailable };

[[nodiscard]] const char *to_string(ContextSourceStatus status) noexcept;

[[nodiscard]] const char *to_string(ContextSuggestionStatus status) noexcept;
[[nodiscard]] ContextSuggestionStatus context_suggestion_status_from_string(const std::string &value) noexcept;

// A proposed Context change - deliberately a distinct type/collection from
// ContextItem (critical invariant: canonical Context and suggestions are
// distinct backend states), never a canonical item carrying a "pending"
// flag. `target_item_id` is set when the suggestion proposes changing an
// existing canonical item; unset means a brand-new item. The resolution
// fields (status/resolved_at/resolved_by_actor_id) are the durable audit
// trail a decision leaves behind - critical invariant: suggestion decisions
// and audit attribution survive restart/recovery.
struct ContextSuggestion {
	std::string id;
	ContextSection section = ContextSection::Other;
	std::string label;
	std::string value;
	std::optional<std::string> target_item_id;
	ContextItemSource source;
	// Backend-resolved link to the same Project Entity Registry identity as
	// the source occurrence. Models never provide this value themselves.
	std::optional<std::string> entity_id;
	ContextSuggestionStatus status = ContextSuggestionStatus::Pending;
	ContextSuggestionReason reason = ContextSuggestionReason::Update;
	std::string created_at;
	std::string resolved_at;
	std::optional<std::string> resolved_by_actor_id;
};

struct ContextMergeConflict {
	std::string id;
	std::string item_id;
	std::string label;
	std::string current_value;
	std::optional<std::string> authored_value;
	std::string current_actor_id;
	std::string authored_actor_id;
	std::string current_timestamp;
	std::string authored_timestamp;
};

} // namespace singularity::context
