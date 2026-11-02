#pragma once

#include "Context/Types/ProjectContext.hpp"
#include "Matter/Types/Matter.hpp"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace singularity::context {

// Read/query access over one Project's canonical Context items and its
// distinct suggestion queue (phase-12-context-model-authority.md's
// "Canonical memory versus suggestions"). Every query is a plain projection
// over `matter.context_items`/`matter.context_suggestions`; mutation lives
// in ContextCuration.hpp so this stays the same read-only shape as
// Privacy/EntityRegistry.hpp.

// Every canonical item, stable creation order (oldest first) so a Context
// section reads top to bottom the same way every time.
[[nodiscard]] std::vector<ContextItem> list_items(const matters::Matter &matter);

// The canonical Project Context visible to AI assembly. Exclusion is
// enforced here in native code and pinned items are returned first while
// preserving stable order inside each priority group.
// Every item a request may use, pinned first: never one excluded from AI, and
// never an automatically derived value whose source document is gone.
[[nodiscard]] std::vector<ContextItem> list_items_for_ai_assembly(const matters::Matter &matter);

// Case-folded (every script), single-spaced text for comparing labels and values.
[[nodiscard]] std::string normalized_context_text(const std::string &value);

// Whether a source still matches its document (see ContextSourceStatus).
[[nodiscard]] ContextSourceStatus context_source_status(const matters::Matter &matter, const ContextItemSource &source);

// How one canonical item stands now: whether its document still holds it, and
// whether requests use it (list_items_for_ai_assembly's rule).
struct ContextItemStanding {
	ContextSourceStatus source_status = ContextSourceStatus::Current;
	bool used_by_ai = true;
};

[[nodiscard]] ContextItemStanding context_item_standing(const matters::Matter &matter, const ContextItem &item);

[[nodiscard]] std::vector<ContextItemSnapshot> snapshot_items_for_ai_assembly(
		const matters::Matter &matter, const std::set<std::string> &included_item_ids);

// Plain local Context search over canonical backend truth. It searches
// labels, values and the existing linked Entity Registry identity; it never
// exposes or fabricates vector similarity scores.
[[nodiscard]] std::vector<ContextSearchResult> search_items(
		const matters::Matter &matter, const std::string &query);

// Pure page-state projection used by the Native query. The production
// caller supplies whether a real automatic Context processor is available;
// assigning a model alone is not enough to claim that processing exists.
[[nodiscard]] ContextPageState context_page_state(
		const matters::Matter &matter, long long conflict_count, bool automatic_updates_available);

[[nodiscard]] std::vector<ContextItem> list_items_by_section(const matters::Matter &matter, ContextSection section);

[[nodiscard]] std::optional<ContextItem> find_item(const matters::Matter &matter, const std::string &item_id);

// At most one canonical item exists per (section, label) - a Context "fact
// slot" is identified by what it is about, not by how many times it has
// been derived or edited. ContextCuration.cpp's submit_derived_value() is
// the only place this lookup decides whether a derived value updates an
// existing slot or creates a new one.
[[nodiscard]] std::optional<ContextItem> find_item_by_label(
		const matters::Matter &matter, ContextSection section, const std::string &label);

// Every suggestion regardless of status - the full audit trail (critical
// invariant: suggestion decisions and audit attribution survive restart/
// recovery), stable creation order.
[[nodiscard]] std::vector<ContextSuggestion> list_suggestions(const matters::Matter &matter);

[[nodiscard]] std::vector<ContextSuggestion> list_pending_suggestions(const matters::Matter &matter);

[[nodiscard]] std::optional<ContextSuggestion> find_suggestion(
		const matters::Matter &matter, const std::string &suggestion_id);

// Stable random ids for a new ContextItem/ContextSuggestion - the backend is
// the only id authority (critical invariant: item/revision/source identity
// ... are backend-owned), never a caller-supplied value.
[[nodiscard]] std::string generate_item_id();
[[nodiscard]] std::string generate_suggestion_id();

// Every durable revision record for one item, oldest first - Phase 13's
// "previous version" audit trail (see ContextItemRevision's own comment).
// ContextCuration.cpp's update_item_value() is the only place that appends
// to this collection.
[[nodiscard]] std::vector<ContextItemRevision> list_item_revisions(
		const matters::Matter &matter, const std::string &item_id);

[[nodiscard]] std::string generate_revision_id();

} // namespace singularity::context
