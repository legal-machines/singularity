#pragma once

#include "Context/SourceMarkers/SourceMarkers.hpp"

#include <optional>
#include <string>
#include <vector>

namespace singularity::matters {

// Stable canonical type ids are persisted on generic Work Products and are
// also the type identity used when the Notes/Issues compatibility foundation
// is projected into the canonical WorkProduct read model. They are strings,
// not a closed enum: Project-owned custom types use the same field.
inline constexpr const char* kWorkProductTypeNote = "canonical.note";
inline constexpr const char* kWorkProductTypeIssue = "canonical.issue";
inline constexpr const char* kWorkProductTypeAnalysis = "canonical.analysis";
inline constexpr const char* kWorkProductTypeDraft = "canonical.draft";
inline constexpr const char* kWorkProductTypeComparison = "canonical.comparison";
inline constexpr const char* kWorkProductTypeObligationSet = "canonical.obligation-set";
inline constexpr const char* kWorkProductTypeChronology = "canonical.chronology";
// The same questions answered across documents (WorkProduct/ReviewTable).
inline constexpr const char* kWorkProductTypeReviewTable = "canonical.review-table";

struct WorkProductType {
	std::string id;
	std::string project_id;
	std::string name;
	std::string description;
	std::optional<std::string> instructions;
	std::optional<std::string> expected_structure;
	bool is_canonical = false;
	long long revision = 1;
	std::string created_at;
	std::string updated_at;
};

// Agent: the report an agent's check kept (Agent/AgentRunner); its reference is
// the analysis operation the check ran, when it ran one. ReviewTable: a Review
// Table kept as a Work Product; its reference is the table.
enum class WorkProductOriginKind { Manual, ChatTask, Finding, Legacy, Agent, ReviewTable };

struct WorkProductOrigin {
	WorkProductOriginKind kind = WorkProductOriginKind::Manual;
	std::optional<std::string> reference_id;
};

// Generic retained professional work. Note and Issue deliberately keep their
// existing specialized contracts and are adapted into the same query model;
// duplicating either object here would create a second source of truth.
// The typed legal objects an operation produces: what a finding, an
// obligation, an event and a proposed change ARE. Declared before the work
// they travel on, because a Work Product now carries its own rows.
struct Finding {
  std::string id;
  std::string matter_id;
  std::string title;
  std::string description;
  std::optional<std::string> recommendation;
  std::optional<std::string> severity;
  std::vector<context::ResolvedSourceReference> source_refs;
};

struct Obligation {
  std::string id;
  std::string matter_id;
  std::optional<std::string> party;
  std::string obligation;
  std::optional<std::string> trigger;
  std::optional<std::string> deadline;
  std::vector<context::ResolvedSourceReference> source_refs;
};

struct Event {
  std::string id;
  std::string matter_id;
  std::optional<std::string> date;
  std::string description;
  std::vector<context::ResolvedSourceReference> source_refs;
};

struct Change {
  std::string id;
  std::string matter_id;
  std::optional<std::string> category;
  std::string description;
  std::vector<context::ResolvedSourceReference> source_refs;
};

struct WorkProduct {
	std::string id;
	std::string project_id;
	std::string type_id;
	std::string title;
	std::string content;
	// The structured universal document this content was rendered from
	// (WorkProductDocument.hpp), as JSON text. Cleared when a person edits the
	// content by hand, so the two can never disagree.
	std::optional<std::string> document;
	std::vector<context::ResolvedSourceReference> source_refs;
	// The typed legal objects the operation produced alongside the prose, kept
	// on the work itself rather than only on the task that made it. A
	// chronology IS its events, an obligation set IS its obligations: a type
	// whose name promises rows is not honest until the rows travel with it -
	// and a task can be retried or deleted while the work it produced stays.
	// Each row keeps its own source references, exactly as the task recorded
	// them; nothing here is re-derived from the text.
	std::vector<Finding> findings;
	std::vector<Obligation> obligations;
	std::vector<Event> events;
	std::vector<Change> changes;
	WorkProductOrigin origin;
	long long revision = 1;
	std::string created_at;
	std::string updated_at;
};

struct WorkProductIssueData {
	std::string description;
	std::string status;
	std::vector<context::ResolvedSourceReference> source_refs;
};

struct WorkProductObject {
	std::string id;
	std::string project_id;
	std::string type_id;
	std::string type_name;
	std::string title;
	std::optional<std::string> content;
	std::optional<std::string> document;
	std::optional<std::string> state;
	std::vector<context::ResolvedSourceReference> source_refs;
	std::optional<WorkProductIssueData> issue;
	// The same typed rows as on WorkProduct above, so a reader of the query
	// model sees the work's own structure without a second lookup.
	std::vector<Finding> findings;
	std::vector<Obligation> obligations;
	std::vector<Event> events;
	std::vector<Change> changes;
	WorkProductOrigin origin;
	long long revision = 1;
	std::string created_at;
	std::string updated_at;
};

// One immutable prior state of a Work Product. Notes and Issues use this
// same record even though their current state remains in their accepted
// specialised collections. `is_current` is a query-only flag and is never
// persisted; detail queries synthesize the current object as the newest
// entry so recovery has one uniform read model.
struct WorkProductRevision {
	std::string id;
	std::string work_product_id;
	std::string project_id;
	std::string type_id;
	std::string type_name;
	std::string title;
	std::optional<std::string> content;
	std::optional<std::string> state;
	std::vector<context::ResolvedSourceReference> source_refs;
	std::optional<WorkProductIssueData> issue;
	WorkProductOrigin origin;
	long long revision = 1;
	std::string created_at;
	bool is_current = false;
};

// Resolved read model over the existing MatterTask/Chat evidence. This is
// not another Observe record: it carries stable links and the small human
// summary Work Product needs, while the exact operation remains the task id.
struct WorkProductProvenance {
	bool is_ai_derived = false;
	std::string origin_label;
	std::string operation_state = "not_applicable";
	std::optional<std::string> operation_id;
	std::string operation_type;
	std::string operation_role;
	std::string operation_status;
	std::string created_at;
	std::optional<std::string> chat_id;
	std::optional<std::string> chat_title;
	std::string chat_state = "not_applicable";
	std::optional<std::string> provider_id;
	std::optional<std::string> model_id;
	std::string execution_kind;
};

struct WorkProductDetail {
	WorkProductObject object;
	std::vector<WorkProductRevision> revisions;
	WorkProductProvenance provenance;
};

[[nodiscard]] const char* to_string(WorkProductOriginKind kind) noexcept;
[[nodiscard]] std::optional<WorkProductOriginKind> work_product_origin_kind_from_string(
		const std::string& value) noexcept;

} // namespace singularity::matters
