#include "Context/ContextRegistry/ContextRegistry.hpp"

#include "Platform/TextFold/TextFold.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"

#include "Privacy/EntityRegistry/EntityRegistry.hpp"

#include <algorithm>
#include <cctype>

namespace singularity::context {
namespace {

// 24 digits that are 96 bits from the system's secure source
// (Platform/SecureRandom).
std::string generate_id(const char *prefix) {
	return std::string(prefix) + platform::secure_random_hex(12);
}

std::string search_text(const std::string &value) {
	return platform::folded_text(value);
}

bool contains_query(const std::string &value, const std::string &query) {
	return search_text(value).find(query) != std::string::npos;
}

} // namespace

const std::vector<ContextSection> &all_context_sections() {
	static const std::vector<ContextSection> sections = {
			ContextSection::Parties,
			ContextSection::Roles,
			ContextSection::KeyPeople,
			ContextSection::Jurisdiction,
			ContextSection::TransactionType,
			ContextSection::KeyDocuments,
			ContextSection::Definitions,
			ContextSection::Dates,
			ContextSection::Amounts,
			ContextSection::GroupStructure,
			ContextSection::KeyFacts,
			ContextSection::UserTasks,
			ContextSection::WorkingAssumptions,
			ContextSection::MaterialConclusions,
			ContextSection::ProjectInstructions,
			ContextSection::PinnedInformation,
			ContextSection::Other,
	};
	return sections;
}

const char *to_string(ContextSection section) noexcept {
	switch (section) {
		case ContextSection::Parties: return "parties";
		case ContextSection::Roles: return "roles";
		case ContextSection::KeyPeople: return "key_people";
		case ContextSection::Jurisdiction: return "jurisdiction";
		case ContextSection::TransactionType: return "transaction_type";
		case ContextSection::KeyDocuments: return "key_documents";
		case ContextSection::Definitions: return "definitions";
		case ContextSection::Dates: return "dates";
		case ContextSection::Amounts: return "amounts";
		case ContextSection::GroupStructure: return "group_structure";
		case ContextSection::KeyFacts: return "key_facts";
		case ContextSection::UserTasks: return "user_tasks";
		case ContextSection::WorkingAssumptions: return "working_assumptions";
		case ContextSection::MaterialConclusions: return "material_conclusions";
		case ContextSection::ProjectInstructions: return "project_instructions";
		case ContextSection::PinnedInformation: return "pinned_information";
		case ContextSection::Other: return "other";
	}
	return "other";
}

ContextSection context_section_from_string(const std::string &value) noexcept {
	for (const ContextSection section : all_context_sections()) {
		if (value == to_string(section)) return section;
	}
	return ContextSection::Other;
}

std::string display_section_name(ContextSection section) {
	switch (section) {
		case ContextSection::Parties: return "Parties";
		case ContextSection::Roles: return "Roles of parties";
		case ContextSection::KeyPeople: return "Key people";
		case ContextSection::Jurisdiction: return "Jurisdiction";
		case ContextSection::TransactionType: return "Transaction / dispute type";
		case ContextSection::KeyDocuments: return "Key documents";
		case ContextSection::Definitions: return "Definitions";
		case ContextSection::Dates: return "Dates";
		case ContextSection::Amounts: return "Amounts";
		case ContextSection::GroupStructure: return "Group structure";
		case ContextSection::KeyFacts: return "Key facts";
		case ContextSection::UserTasks: return "User tasks";
		case ContextSection::WorkingAssumptions: return "Working assumptions";
		case ContextSection::MaterialConclusions: return "Material conclusions";
		case ContextSection::ProjectInstructions: return "Project instructions";
		case ContextSection::PinnedInformation: return "Pinned information";
		case ContextSection::Other: return "Other";
	}
	return "Other";
}

const char *to_string(ContextItemAuthority authority) noexcept {
	switch (authority) {
		case ContextItemAuthority::AutomaticallyDerived: return "automatically_derived";
		case ContextItemAuthority::UserConfirmed: return "user_confirmed";
		case ContextItemAuthority::UserAuthored: return "user_authored";
		case ContextItemAuthority::Locked: return "locked";
	}
	return "automatically_derived";
}

ContextItemAuthority context_item_authority_from_string(const std::string &value) noexcept {
	if (value == "user_confirmed") return ContextItemAuthority::UserConfirmed;
	if (value == "user_authored") return ContextItemAuthority::UserAuthored;
	if (value == "locked") return ContextItemAuthority::Locked;
	return ContextItemAuthority::AutomaticallyDerived;
}

const char *to_string(ContextItemKind kind) noexcept {
	switch (kind) {
		case ContextItemKind::Fact: return "fact";
		case ContextItemKind::WorkingAssumption: return "working_assumption";
		case ContextItemKind::Instruction: return "instruction";
		case ContextItemKind::Conclusion: return "conclusion";
	}
	return "fact";
}

ContextItemKind context_item_kind_from_string(const std::string &value) noexcept {
	if (value == "working_assumption") return ContextItemKind::WorkingAssumption;
	if (value == "instruction") return ContextItemKind::Instruction;
	if (value == "conclusion") return ContextItemKind::Conclusion;
	return ContextItemKind::Fact;
}

ContextItemKind default_context_item_kind(ContextSection section) noexcept {
	if (section == ContextSection::WorkingAssumptions) return ContextItemKind::WorkingAssumption;
	if (section == ContextSection::ProjectInstructions) return ContextItemKind::Instruction;
	if (section == ContextSection::MaterialConclusions) return ContextItemKind::Conclusion;
	return ContextItemKind::Fact;
}

const char *to_string(ContextSourceKind kind) noexcept {
	switch (kind) {
		case ContextSourceKind::User: return "user";
		case ContextSourceKind::Document: return "document";
		case ContextSourceKind::Chat: return "chat";
	}
	return "user";
}

ContextSourceKind context_source_kind_from_string(const std::string &value) noexcept {
	if (value == "document") return ContextSourceKind::Document;
	if (value == "chat") return ContextSourceKind::Chat;
	return ContextSourceKind::User;
}

const char *to_string(ContextSuggestionStatus status) noexcept {
	switch (status) {
		case ContextSuggestionStatus::Pending: return "pending";
		case ContextSuggestionStatus::Accepted: return "accepted";
		case ContextSuggestionStatus::Dismissed: return "dismissed";
	}
	return "pending";
}

ContextSuggestionStatus context_suggestion_status_from_string(const std::string &value) noexcept {
	if (value == "accepted") return ContextSuggestionStatus::Accepted;
	if (value == "dismissed") return ContextSuggestionStatus::Dismissed;
	return ContextSuggestionStatus::Pending;
}

const char *to_string(ContextSuggestionReason reason) noexcept {
	switch (reason) {
		case ContextSuggestionReason::NewItem: return "new_item";
		case ContextSuggestionReason::Update: return "update";
		case ContextSuggestionReason::Conflict: return "conflict";
		case ContextSuggestionReason::Removed: return "removed";
	}
	return "update";
}

ContextSuggestionReason context_suggestion_reason_from_string(const std::string &value) noexcept {
	if (value == "new_item") return ContextSuggestionReason::NewItem;
	if (value == "conflict") return ContextSuggestionReason::Conflict;
	if (value == "removed") return ContextSuggestionReason::Removed;
	return ContextSuggestionReason::Update;
}

const char *to_string(ContextSourceStatus status) noexcept {
	switch (status) {
		case ContextSourceStatus::Current: return "current";
		case ContextSourceStatus::Changed: return "changed";
		case ContextSourceStatus::Unavailable: return "unavailable";
	}
	return "current";
}

std::string normalized_context_text(const std::string &value) {
	const std::string folded = platform::folded_text(value);
	std::string result;
	bool pendingSpace = false;
	for (const unsigned char byte : folded) {
		if (std::isspace(byte) != 0) {
			pendingSpace = !result.empty();
			continue;
		}
		if (pendingSpace) result.push_back(' ');
		pendingSpace = false;
		result.push_back(static_cast<char>(byte));
	}
	return result;
}

ContextSourceStatus context_source_status(const matters::Matter &matter, const ContextItemSource &source) {
	if (source.kind != ContextSourceKind::Document || !source.document_reference) return ContextSourceStatus::Current;
	const ResolvedSourceReference &reference = *source.document_reference;
	const auto document = std::find_if(matter.documents.begin(), matter.documents.end(),
			[&](const matters::MatterDocument &candidate) { return candidate.id == reference.document_id; });
	if (document == matter.documents.end()
			|| matters::document_lifecycle_state(*document) == matters::DocumentLifecycleState::Unavailable)
		return ContextSourceStatus::Unavailable;
	// A passage id is "<document id>:<content fingerprint>:<range>": a value
	// taken from other content than the document holds now is stale.
	if (!reference.chunk_id.empty() && !document->fingerprint.empty()
			&& reference.chunk_id.rfind(document->id + ":" + document->fingerprint + ":", 0) != 0)
		return ContextSourceStatus::Changed;
	return ContextSourceStatus::Current;
}

namespace {

// The memory brief folds other values into one text. When a person keeps one of
// them from AI, or removes it, at or after the moment the brief was written,
// the brief may still carry it. Timestamps are kept to the second, so "at"
// counts too: in a tie the brief is held back, the safe side.
bool brief_outdated_by_withdrawal(const matters::Matter &matter, const ContextItem &brief) {
	if (brief.label != kMemoryBriefLabel || brief.authority != ContextItemAuthority::AutomaticallyDerived) return false;
	for (const ContextItem &other : matter.context_items)
		if (other.id != brief.id && other.is_excluded_from_ai && other.updated_at >= brief.updated_at) return true;
	for (const ContextSuggestion &suggestion : matter.context_suggestions)
		if (suggestion.reason == ContextSuggestionReason::Removed && suggestion.label != kMemoryBriefLabel
				&& suggestion.resolved_at >= brief.updated_at)
			return true;
	return false;
}

} // namespace

ContextItemStanding context_item_standing(const matters::Matter &matter, const ContextItem &item) {
	ContextItemStanding standing;
	if (item.source) standing.source_status = context_source_status(matter, *item.source);
	// An automatically derived value whose document is gone no longer speaks for
	// the Project: it is held back until a document states it again or a person
	// confirms it. Every other item stays, and its source status (a document
	// changed since the value was taken) travels with it into the request.
	standing.used_by_ai = !item.is_excluded_from_ai
		&& !(item.authority == ContextItemAuthority::AutomaticallyDerived && !item.is_locked
			&& standing.source_status == ContextSourceStatus::Unavailable);
	// A brief that may still carry a value since withdrawn from AI is held back
	// from every request until the memory keeper writes it again without it.
	if (standing.used_by_ai && brief_outdated_by_withdrawal(matter, item)) standing.used_by_ai = false;
	return standing;
}

const char *to_string(ContextSurfaceState state) noexcept {
	switch (state) {
		case ContextSurfaceState::Empty: return "empty";
		case ContextSurfaceState::Building: return "building";
		case ContextSurfaceState::Partial: return "partial";
		case ContextSurfaceState::Conflict: return "conflict";
		case ContextSurfaceState::Ready: return "ready";
	}
	return "empty";
}

std::vector<ContextItem> list_items(const matters::Matter &matter) {
	return matter.context_items;
}

std::vector<ContextItem> list_items_for_ai_assembly(const matters::Matter &matter) {
	const auto usable = [&](const ContextItem &item) { return context_item_standing(matter, item).used_by_ai; };
	std::vector<ContextItem> result;
	for (const ContextItem &item : matter.context_items) {
		if (usable(item) && item.is_pinned == true) result.push_back(item);
	}
	for (const ContextItem &item : matter.context_items) {
		if (usable(item) && item.is_pinned == false) result.push_back(item);
	}
	return result;
}

std::vector<ContextItemSnapshot> snapshot_items_for_ai_assembly(
		const matters::Matter &matter, const std::set<std::string> &included_item_ids) {
	std::vector<ContextItemSnapshot> result;
	for (const ContextItem &item : list_items_for_ai_assembly(matter)) {
		if (included_item_ids.count(item.id) == 0) continue;
		result.push_back(ContextItemSnapshot{
				item.id, item.revision, item.section, item.kind, item.label, item.value, item.authority, item.source,
				item.entity_id, item.is_pinned, item.is_locked, item.is_excluded_from_ai});
	}
	return result;
}

std::vector<ContextSearchResult> search_items(const matters::Matter &matter, const std::string &query) {
	const std::string normalized_query = search_text(query);
	if (normalized_query.find_first_not_of(" \t\r\n") == std::string::npos) return {};

	std::vector<ContextSearchResult> result;
	for (const ContextItem &item : matter.context_items) {
		bool is_match = contains_query(item.label, normalized_query) || contains_query(item.value, normalized_query)
				|| contains_query(display_section_name(item.section), normalized_query)
				|| contains_query(to_string(item.kind), normalized_query);
		if (is_match == false && item.entity_id) {
			const auto entity = privacy::find_entity(matter, *item.entity_id);
			if (entity) {
				is_match = contains_query(entity->pseudonym, normalized_query)
						|| contains_query(entity->canonical_value, normalized_query);
				for (const std::string &alias : entity->aliases) {
					if (contains_query(alias, normalized_query) == true) is_match = true;
				}
			}
		}
		if (is_match == true) result.push_back({item.id, item.section, item.label, item.value});
	}
	return result;
}

ContextPageState context_page_state(
		const matters::Matter &matter, long long conflict_count, bool automatic_updates_available) {
	ContextPageState result;
	result.item_count = static_cast<long long>(matter.context_items.size());
	result.pending_suggestion_count = static_cast<long long>(list_pending_suggestions(matter).size());
	result.conflict_count = conflict_count;
	result.automatic_updates_available = automatic_updates_available;
	bool has_processing_documents = false;
	bool has_attention_documents = false;
	for (const matters::MatterDocument &document : matter.documents) {
		const matters::DocumentLifecycleState state = matters::document_lifecycle_state(document);
		if (state == matters::DocumentLifecycleState::Unsupported
				|| state == matters::DocumentLifecycleState::Unavailable) {
			continue;
		}
		result.total_document_count++;
		if (state == matters::DocumentLifecycleState::Ready) result.processed_document_count++;
		if (state == matters::DocumentLifecycleState::Processing || state == matters::DocumentLifecycleState::Changed) {
			has_processing_documents = true;
		}
		if (state == matters::DocumentLifecycleState::NeedsAttention) has_attention_documents = true;
	}

	if (conflict_count > 0) result.state = ContextSurfaceState::Conflict;
	else if (automatic_updates_available == true && has_processing_documents == true) {
		result.state = ContextSurfaceState::Building;
	} else if (result.item_count == 0) result.state = ContextSurfaceState::Empty;
	else if (automatic_updates_available == false || has_attention_documents == true) {
		result.state = ContextSurfaceState::Partial;
	} else result.state = ContextSurfaceState::Ready;
	return result;
}

std::vector<ContextItem> list_items_by_section(const matters::Matter &matter, ContextSection section) {
	std::vector<ContextItem> result;
	for (const ContextItem &item : matter.context_items) {
		if (item.section == section) result.push_back(item);
	}
	return result;
}

std::optional<ContextItem> find_item(const matters::Matter &matter, const std::string &item_id) {
	const auto found = std::find_if(matter.context_items.begin(), matter.context_items.end(),
			[&](const ContextItem &item) { return item.id == item_id; });
	if (found == matter.context_items.end()) return std::nullopt;
	return *found;
}

std::optional<ContextItem> find_item_by_label(
		const matters::Matter &matter, ContextSection section, const std::string &label) {
	// One slot per section and label, whatever its case or spacing: an
	// automatic "Purchase Price" is the same fact as a person's "Purchase price".
	const std::string normalized = normalized_context_text(label);
	const auto found = std::find_if(matter.context_items.begin(), matter.context_items.end(),
			[&](const ContextItem &item) { return item.section == section && normalized_context_text(item.label) == normalized; });
	if (found == matter.context_items.end()) return std::nullopt;
	return *found;
}

std::vector<ContextSuggestion> list_suggestions(const matters::Matter &matter) {
	return matter.context_suggestions;
}

std::vector<ContextSuggestion> list_pending_suggestions(const matters::Matter &matter) {
	std::vector<ContextSuggestion> result;
	for (const ContextSuggestion &suggestion : matter.context_suggestions) {
		if (suggestion.status == ContextSuggestionStatus::Pending) result.push_back(suggestion);
	}
	return result;
}

std::optional<ContextSuggestion> find_suggestion(const matters::Matter &matter, const std::string &suggestion_id) {
	const auto found = std::find_if(matter.context_suggestions.begin(), matter.context_suggestions.end(),
			[&](const ContextSuggestion &suggestion) { return suggestion.id == suggestion_id; });
	if (found == matter.context_suggestions.end()) return std::nullopt;
	return *found;
}

std::string generate_item_id() { return generate_id("ctxi_"); }
std::string generate_suggestion_id() { return generate_id("ctxs_"); }

std::vector<ContextItemRevision> list_item_revisions(const matters::Matter &matter, const std::string &item_id) {
	std::vector<ContextItemRevision> result;
	for (const ContextItemRevision &revision : matter.context_item_revisions) {
		if (revision.item_id == item_id) result.push_back(revision);
	}
	return result;
}

std::string generate_revision_id() { return generate_id("ctxr_"); }

} // namespace singularity::context
