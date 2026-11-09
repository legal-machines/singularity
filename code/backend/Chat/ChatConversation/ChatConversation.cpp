#include "Chat/ChatConversation/ChatConversation.hpp"
#include "Chat/MessageProvenance/MessageProvenance.hpp"
#include "Chat/ChatContinuity/ChatContinuity.hpp"
#include "Context/ComputedStatements/ComputedStatements.hpp"

#include "Document/DocumentSourceResolver/DocumentSourceResolver.hpp"
#include "Document/Types/DocumentActionEvidence.hpp"
#include "ExternalResearch/ResearchTools/ResearchTools.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Provider/Types/TaskError.hpp"
#include "Provider/Types/UsageMetadata.hpp"
#include "Routing/RoutingLog/RoutingLog.hpp"
#include "Tool/Types/ToolTypes.hpp"
#include "WorkProduct/WorkflowDefinitions/WorkflowDefinitions.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace singularity::chat {
namespace {

// One Project, read on its own: reading a Chat never waits on, or touches, any
// other Project of the workspace. One whose data can't be read just now is
// refused as such, never answered as a Chat no longer there.
matters::Matter require_project(const std::string &project_id) {
  std::optional<matters::Matter> project =
      matters::load_matter(node::require_active_workspace_path(), project_id);
  if (!project) throw std::runtime_error("This Project is no longer available.");
  matters::require_readable(*project);
  return std::move(*project);
}

// The recorded reference this exact operation cited, from the answer or from
// one of its structured results (a finding, obligation, event or change,
// each validated against the same supplied sources when it was written).
const context::ResolvedSourceReference *recorded_reference(const matters::MatterTask &task,
    const std::string &document_id, const std::string &chunk_id) {
  const auto matches = [&](const context::ResolvedSourceReference &entry) {
    return entry.document_id == document_id && entry.chunk_id == chunk_id;
  };
  const auto in = [&](const std::vector<context::ResolvedSourceReference> &list) -> const context::ResolvedSourceReference * {
    const auto found = std::find_if(list.begin(), list.end(), matches);
    return found != list.end() ? &*found : nullptr;
  };
  if (const auto *found = in(task.source_references)) return found;
  for (const auto &entry : task.findings) if (const auto *found = in(entry.source_refs)) return found;
  for (const auto &entry : task.obligations) if (const auto *found = in(entry.source_refs)) return found;
  for (const auto &entry : task.events) if (const auto *found = in(entry.source_refs)) return found;
  for (const auto &entry : task.changes) if (const auto *found = in(entry.source_refs)) return found;
  return nullptr;
}

const matters::MatterChat &require_chat(const matters::Matter &project, const std::string &chat_id) {
  const auto found = std::find_if(project.chats.begin(), project.chats.end(),
      [&](const matters::MatterChat &entry) { return entry.id == chat_id; });
  if (found == project.chats.end()) throw std::runtime_error("This Chat is no longer available.");
  return *found;
}

// How many times the change this task wrote carries the unverified-source
// label - counted on the written side only (what was added), never on the
// document's own wording it replaced or anchored to, and each written value
// once (the top-level text repeats the steps).
std::size_t unverified_labels_written(const matters::MatterTask &task) {
  if (!task.artifact_change) return 0;
  const nlohmann::json change = nlohmann::json::parse(*task.artifact_change, nullptr, false);
  if (!change.is_object()) return 0;
  const std::string label = context::kUnverifiedSourceLabel;
  const auto count_in = [&](const nlohmann::json &value) -> std::size_t {
    if (!value.is_string()) return 0;
    const std::string text = value.get<std::string>();
    std::size_t count = 0;
    for (std::size_t at = text.find(label); at != std::string::npos; at = text.find(label, at + label.size())) ++count;
    return count;
  };
  std::size_t count = 0;
  if (change.contains("operations") && change.at("operations").is_array() && !change.at("operations").empty()) {
    for (const nlohmann::json &step : change.at("operations")) {
      if (!step.is_object()) continue;
      // A formatting step re-writes the document's own words: nothing was added.
      if (step.value("kind", std::string()) != "format" && step.contains("addedText")) count += count_in(step.at("addedText"));
      if (step.value("kind", std::string()) == "row_insert" && step.contains("cells") && step.at("cells").is_array())
        for (const nlohmann::json &cell : step.at("cells")) count += count_in(cell);
    }
    return count;
  }
  if (change.contains("addedText")) count += count_in(change.at("addedText"));
  return count;
}

std::string source_reference_status_for(std::size_t citation_count, std::size_t unresolved_count) {
  if (unresolved_count > 0) return "partial";
  if (citation_count > 0) return "checked";
  return "none";
}

// The real, physically-mutated Project document this operation's own
// evidence recorded - never inferred from response text. An operation
// targets at most one connected document today, so the first recorded
// mutation is the whole fact. Covers both real mutation paths: the older
// connected-document-action's own AppliedDocumentActionEvidence, and
// DirectDocumentWork's document.patch.apply call (which records only
// ToolCallEvidence, never a second AppliedDocumentActionEvidence
// mechanism - see MatterRequestExecution.cpp's own comment on why. The
// resulting version is read live from the Project's own current Document
// Registry entry rather than stored a second time on MatterTask, so it is
// always the real current version even if the document changed again since.
std::optional<ChatDocumentChange> document_change_from_task(
    const matters::MatterTask &task, const matters::Matter &project) {
  // The change as written, when this operation recorded one. A generic
  // ProfessionalArtifactAction the lawyer approved records BOTH identities -
  // the apply evidence below and its own artifact fields - so carrying these
  // onto whichever identity is found is what keeps the written change on
  // screen after approval instead of leaving the turn with a bare version
  // number and nothing to show.
  const auto with_artifact = [&](ChatDocumentChange change) {
    if (task.artifact_document_id && *task.artifact_document_id == change.document_id) {
      change.artifact_title = task.artifact_title;
      change.artifact_format = task.artifact_format;
      change.artifact_change_kind = task.artifact_change_kind;
      change.artifact_change = task.artifact_change;
    }
    return change;
  };
  if (!task.document_action_evidence.empty()) {
    const documents::AppliedDocumentActionEvidence &evidence = task.document_action_evidence.front();
    return with_artifact(ChatDocumentChange{evidence.document_id, evidence.resulting_version});
  }
  if (task.direct_document_work_document_id) {
    const auto document = std::find_if(project.documents.begin(), project.documents.end(),
        [&](const matters::MatterDocument &entry) { return entry.id == *task.direct_document_work_document_id; });
    if (document != project.documents.end()) return with_artifact(ChatDocumentChange{document->id, document->version});
  }
  // Generic ProfessionalArtifactAction result (Section 15's own UI contract
  // gap): the ONLY one of these three real mutation identities that also
  // carries a title/format/change-kind the frontend can present without
  // reverse-engineering it from raw structural fields.
  if (task.artifact_document_id) {
    ChatDocumentChange change{*task.artifact_document_id, task.artifact_resulting_version.value_or(0)};
    change.artifact_title = task.artifact_title;
    change.artifact_format = task.artifact_format;
    change.artifact_change_kind = task.artifact_change_kind;
    change.artifact_change = task.artifact_change;
    return change;
  }
  return std::nullopt;
}

ChatExecutionSummary execution_summary_from_task(const matters::MatterTask &task,
    std::size_t source_reference_count, const routing::RoutingLogEntry *routing_log_entry) {
  ChatExecutionSummary summary;
  summary.primary_role = task.operation_role;
  summary.primary_provider_id = task.provider_id;
  summary.primary_model_id = task.model_id;
  summary.execution_location = task.execution_kind;
  summary.protection_applied = task.protected_api_applied;
  summary.source_reference_count = source_reference_count;
  // Web research the turn ran on a hosted model is charged on its own (the
  // tool call's external_usage, which Usage lists as its own line), and it is
  // part of what this turn cost, so the one cost rule folds it in. A local
  // model's own work is free and has no price: folded in, it would make every
  // researched local turn "partial", so its tokens are added beside it instead.
  const bool local = task.execution_kind == "local";
  providers::UsageMetadata usage = local ? providers::UsageMetadata{} : task.usage;
  for (const tools::ToolCallEvidence &call : task.tool_call_evidence) {
    if (!call.external_usage) continue;
    usage = providers::accumulate_usage(usage, *call.external_usage);
    summary.paid_external_calls = true;
  }
  if (local) {
    const auto add = [](std::optional<long long> &target, const std::optional<long long> &value) {
      if (value) target = target.value_or(0) + *value;
    };
    add(usage.input_tokens, task.usage.input_tokens);
    add(usage.output_tokens, task.usage.output_tokens);
    add(usage.total_tokens, task.usage.total_tokens);
  }
  summary.input_tokens = usage.input_tokens;
  summary.output_tokens = usage.output_tokens;
  summary.total_tokens = usage.total_tokens;
  // UsageMetadata's own rule: monetary evidence is populated only when the
  // provider response supplied a real amount. A total some of whose calls came
  // back unpriced is "partial" (accumulate_usage) - a smaller bill than the
  // real one, so it is not shown as this turn's cost.
  if (usage.cost_micros && usage.cost_classification != std::optional<std::string>("partial")) {
    summary.cost_micros = usage.cost_micros;
    summary.cost_currency = usage.cost_currency;
  }
  if (routing_log_entry != nullptr) {
    summary.latency_ms = routing_log_entry->latency_ms;
    summary.routing_reason = routing_log_entry->routing_reason;
  }
  // A change the lawyer was asked about ran twice - preparing it, then writing
  // it once approved - and only the second run is always logged. Its own
  // events measure both runs and leave out the wait for the decision.
  if (waited_for_approval(task)) {
    const std::optional<long long> worked = worked_milliseconds(task);
    if (worked && (!summary.latency_ms || *worked > *summary.latency_ms)) summary.latency_ms = worked;
  }
  // A question put to the lawyer holds its run open until they answer, so that
  // run's logged time holds the wait as well; its own events leave it out.
  if (asked_the_lawyer(task)) {
    if (const std::optional<long long> worked = worked_milliseconds(task)) summary.latency_ms = worked;
  }
  return summary;
}

// Reads the real typed Professional Completion Challenge verdict straight
// out of the durable checkpoint ConnectedDocumentAction already persists
// (connected_document_draft_checkpoint_json()'s own "professionalReview" /
// "professionalReviewReviewedRevision" / "revision" keys) - never a second
// review record. Projects std::nullopt for every dishonest case: no
// connected document operation, no review yet, or a verdict recorded
// against a revision that is no longer current (professional_review_
// passed_current_revision()'s own rule, mirrored here for display).
std::optional<ChatProfessionalReview> professional_review_from_task(const matters::MatterTask &task) {
  if (!task.document_operation_checkpoint) return std::nullopt;
  const nlohmann::json &draft = task.document_operation_checkpoint->draft;
  if (!draft.is_object()) return std::nullopt;
  const auto reviewIt = draft.find("professionalReview");
  if (reviewIt == draft.end() || !reviewIt->is_object()) return std::nullopt;
  const std::uint64_t reviewedRevision = draft.value("professionalReviewReviewedRevision", std::uint64_t{0});
  const std::uint64_t currentRevision = draft.value("revision", std::uint64_t{0});
  if (reviewedRevision != currentRevision) return std::nullopt;

  ChatProfessionalReview review;
  review.verdict = reviewIt->value("verdict", std::string("pass"));
  review.reviewed_revision = reviewedRevision;
  if (reviewIt->contains("issues") && reviewIt->at("issues").is_array()) {
    for (const nlohmann::json &issue : reviewIt->at("issues")) {
      review.issues.push_back(ChatProfessionalReviewIssue{
          issue.value("category", std::string()),
          issue.value("message", std::string()),
          issue.value("suggestedNextAction", std::string())});
    }
  }
  // Real Evidence Objectives the reviewer's own model declared and inspected
  // (Document/ConnectedDocumentAction.hpp's own persisted "evidenceObjectives"
  // checkpoint field) - never a computed completeness percentage.
  if (draft.contains("evidenceObjectives") && draft.at("evidenceObjectives").is_array()) {
    for (const nlohmann::json &objective : draft.at("evidenceObjectives")) {
      ChatEvidenceObjective evidenceObjective;
      evidenceObjective.id = objective.value("id", std::string());
      evidenceObjective.description = objective.value("description", std::string());
      evidenceObjective.table_handle = objective.value("tableHandle", std::string());
      evidenceObjective.coverage_mode = objective.value("coverageMode", std::string("targeted"));
      evidenceObjective.closed = objective.value("closed", false);
      if (objective.contains("inspectedRanges") && objective.at("inspectedRanges").is_array()) {
        for (const nlohmann::json &range : objective.at("inspectedRanges")) {
          evidenceObjective.inspected_ranges.push_back(ChatEvidenceCoverageRange{
              range.value("startRow", std::size_t{0}), range.value("endRow", std::size_t{0})});
        }
      }
      review.evidence_objectives.push_back(std::move(evidenceObjective));
    }
  }
  return review;
}

// Whether an operation has finished, and so is a turn of its Chat. One still
// working, waiting for the lawyer or interrupted is unfinished work
// (chat::list_operations), not yet a turn.
bool finished_operation(const matters::MatterTask &task) {
  return task.operation_execution_state == "completed" || task.operation_execution_state == "failed"
      || task.operation_execution_state == "stopped" || task.operation_execution_state == "conflict";
}

ChatTurn turn_from_task(const matters::MatterTask &task, const matters::Matter &project,
    const routing::RoutingLogEntry *routing_log_entry) {
  ChatTurn turn;
  turn.task_id = task.id;
  turn.created_at = task.created_at;
  turn.prompt = task.prompt;
  turn.agent_id = task.agent_id;
  turn.is_agent_message = task.operation_type == "agent_message";
  turn.agent_initiated = task.agent_initiated;
  turn.is_member_message = task.operation_type == "member_message";
  if (turn.is_member_message)
    turn.message_provenance = to_string(member_message_provenance(project.id, task));
  turn.author_actor_id = task.author_actor_id;
  turn.author_name = task.author_name;
  turn.mentions = task.mentions;
  turn.answers_message_id = task.answers_message_id;
  turn.quote_message_id = task.quote_message_id;
  turn.attachment_document_ids = task.attachment_document_ids;
  turn.reactions = task.reactions;
  if (turn.is_member_message) turn.received = task.received;
  turn.edited_at = task.edited_at;
  turn.deleted_at = task.deleted_at;
  turn.not_checked = task.not_checked;
  turn.ask_options = task.ask_options;
  turn.answer_option = task.answer_option;
  turn.thread_event = task.thread_event;
  turn.place = task.place;
  // Asked of the identity only for a message this computer signed. A thread's
  // closing or reopening stays as it was made.
  turn.can_change = turn.is_member_message && task.deleted_at.empty() && task.thread_event.empty()
      && turn.message_provenance == "this_computer"
      && task.author_actor_id == platform::resolve_actor_identity().actor_id;
  turn.status = task.stopped_by_user == true || task.operation_execution_state == "stopped" ? "stopped"
				: task.operation_execution_state == "failed" || task.operation_execution_state == "conflict"
					? "failed"
				: task.status == matters::TaskStatus::Error ? "failed" : "completed";
  turn.legal_task_preset_id = task.legal_task_preset_id;
  turn.legal_task_preset_version = task.legal_task_preset_version;
  turn.selected_document_ids = task.selected_document_ids;
  turn.source_policy_mode = task.selected_document_ids.empty() ? "relevant" : "selected";
  // A request that failed before anything was sent has no model that answered,
  // but it always has the model it was to run on: a Consilium seat that never
  // reached its provider still says which model that was.
  turn.provider_id = task.provider_id ? task.provider_id : task.requested_provider_id;
  turn.model_id = task.model_id ? task.model_id : task.requested_model_id;
  turn.execution_kind = task.execution_kind;
  turn.protected_api_mode = task.protected_api_mode;
  turn.protected_api_applied = task.protected_api_applied;
	turn.research_mode = task.research_mode;
  turn.response_integrity = task.privacy_metadata.response_integrity;
  // Under the protected computation protocol a figure the answer contract
  // could not verify was left out (answer.figure_withheld); the lawyer is told
  // on the same line as a figure nothing traces.
  for (const matters::AgentOperationEvent &event : task.operation_events)
    if (event.type == "answer.figure_unresolved" || event.type == "answer.figure_withheld")
      turn.unresolved_figure_notice = event.message;
  turn.protected_value_count = task.privacy_metadata.protected_count;
  turn.protected_category_counts = task.privacy_metadata.category_counts;
  turn.error = task.error;
  if (task.error_code) turn.error_code = providers::to_string(*task.error_code);
  turn.context_budget_omitted_material = task.context_budget_excluded_material;
  turn.request_coverage = task.request_coverage;
  // Phase 27's source-problem Error - real regardless of whether this
  // attempt still completed using other, usable sources (never gated by
  // turn.status, since "… Chat … usable
  // sources" is exactly the honest case: the rest of the answer succeeded).
  turn.unavailable_source_documents = task.unavailable_source_documents;

  // Phase 26's real Consilium/workflow operation linkage - direct MatterTask
  // provenance, never re-derived or guessed from response content.
  turn.consilium_group_id = task.consilium_group_id;
  if (task.consilium_role) turn.consilium_role = matters::to_string(*task.consilium_role);
  turn.workflow_run_id = task.workflow_run_id;
  turn.workflow_id = task.workflow_id;
  turn.workflow_version = task.workflow_version;
  turn.workflow_step_id = task.workflow_step_id;
  if (task.workflow_id && task.workflow_version) {
    const auto workflow = context::find_workflow_definition(*task.workflow_id, *task.workflow_version);
    if (workflow) {
      turn.workflow_name = workflow->name;
      if (task.workflow_step_id) {
        const auto step = std::find_if(workflow->steps.begin(), workflow->steps.end(),
            [&](const context::WorkflowStepDefinition &entry) { return entry.id == *task.workflow_step_id; });
        if (step != workflow->steps.end()) turn.workflow_step_name = step->name;
      }
    }
  }

  // A stopped or failed operation is never rendered as an answer, so its
  // response is not projected at all - the same rule that keeps a
  // response-integrity failure (already an error MatterTask, see
  // ProtectedInference.cpp) from ever appearing as normal successful work.
  // The change a lawyer declined ends the turn without an answer, and is what
  // the turn shows.
  turn.rejected_change = task.rejected_approval;
  // What Singularity asked the lawyer while it worked belongs to the turn however
  // the turn ended. A question the operation finished without an answer to was
  // never answered, and says so.
  turn.questions = task.lawyer_questions;
  if (finished_operation(task))
    for (matters::LawyerQuestionRecord &record : turn.questions)
      if (record.state == "asked") record.state = "unanswered";
  if (turn.status != "completed" || !task.response_text) {
    turn.source_reference_status = source_reference_status_for(0, 0);
    return turn;
  }

  // The backend owns every step from persisted response text to render-safe
  // segments. Only a reference this operation itself recorded as supplied
  // and cited can become a citation here; anything else is counted.
  // The arithmetic the model asked Singularity to work out on this computer is
  // worked out here, where the lawyer reads it - the stored answer, which later
  // requests read, keeps it as the model asked (Context/ComputedStatements).
  const research::ExternalCitationProjection external = research::project_external_citations(
	  context::computed_statements::compute(*task.response_text), task.external_evidence);
  const context::ProjectedResponse projected =
      context::project_response_references(external.plain_text, task.source_references);
  turn.unresolved_reference_count = projected.unresolved_reference_count + external.unresolved_count;
  turn.source_reference_status =
	  source_reference_status_for(projected.citations.size() + external.citations.size(),
		  turn.unresolved_reference_count);
  for (std::size_t index = 0; index < projected.citations.size(); ++index) {
    const context::ResolvedSourceReference &reference = projected.citations[index];
    ChatCitation citation;
    citation.number = static_cast<int>(index + 1);
    citation.document_id = reference.document_id;
    citation.chunk_id = reference.chunk_id;
    citation.document_name = reference.document_name;
    citation.page = reference.page;
    citation.heading = reference.heading;
    citation.label = context::format_reference_label(reference);
    turn.citations.push_back(std::move(citation));
  }
	for (const research::ExternalEvidenceReference &reference : external.citations) {
		ChatCitation citation;
		citation.number = static_cast<int>(turn.citations.size() + 1);
		citation.source_kind = "external";
		citation.evidence_id = reference.evidence_id;
		citation.url = reference.url;
		citation.domain = reference.domain;
		citation.source_class = research::to_string(reference.source_class);
		citation.excerpt = reference.excerpt;
		citation.label = reference.title.value_or(reference.domain.value_or("External source"));
		turn.citations.push_back(std::move(citation));
	}
	// What an AI colleague's work read on the web and its words do not
	// cite: listed with number 0, as what the work searched - never set
	// beside any words.
	if (turn.is_agent_message)
		for (const research::ExternalEvidenceReference &reference : task.external_evidence) {
			const bool isCited = std::any_of(external.citations.begin(), external.citations.end(),
				[&](const research::ExternalEvidenceReference &cited) { return cited.evidence_id == reference.evidence_id; });
			if (isCited) continue;
			ChatCitation citation;
			citation.number = 0;
			citation.source_kind = "external";
			citation.evidence_id = reference.evidence_id;
			citation.url = reference.url;
			citation.domain = reference.domain;
			citation.source_class = research::to_string(reference.source_class);
			citation.excerpt = reference.excerpt;
			citation.label = reference.title.value_or(reference.domain.value_or("External source"));
			turn.citations.push_back(std::move(citation));
		}
	// An answer that cited nothing inline, from an operation that really did read
	// the Project's documents, used to be shown as an answer with no sources at
	// all - so prose looked unsupported beside a typed result carrying the very
	// same references. What this operation actually supplied and relied on is
	// recorded on the task; it is listed here, with number 0, which says exactly
	// what is true: these are the sources this answer was written from, and Singularity
	// cannot say which sentence each one stands behind. Nothing is matched
	// to text, nothing is renumbered, and a turn that did cite inline is
	// untouched.
	if (turn.citations.empty() && !task.source_references.empty()) {
		for (const context::ResolvedSourceReference &reference : task.source_references) {
			ChatCitation citation;
			citation.number = 0;
			citation.document_id = reference.document_id;
			citation.chunk_id = reference.chunk_id;
			citation.document_name = reference.document_name;
			citation.page = reference.page;
			citation.heading = reference.heading;
			citation.label = context::format_reference_label(reference);
			turn.citations.push_back(std::move(citation));
		}
	}
	for (const context::ProjectedResponseSegment &segment : projected.segments) {
		if (segment.citation_number > 0) {
			turn.response_segments.push_back(ChatResponseSegment{"", segment.citation_number});
			continue;
		}
		std::size_t position = 0;
		while (position < segment.text.size()) {
			const std::size_t start = segment.text.find('\x1e', position);
			if (start == std::string::npos) {
				turn.response_segments.push_back(ChatResponseSegment{segment.text.substr(position), 0});
				break;
			}
			if (start > position)
				turn.response_segments.push_back(ChatResponseSegment{segment.text.substr(position, start - position), 0});
			const std::size_t end = segment.text.find('\x1f', start + 1);
			if (end == std::string::npos) break;
			const std::size_t externalIndex = static_cast<std::size_t>(std::stoul(
				segment.text.substr(start + 1, end - start - 1)));
			turn.response_segments.push_back(ChatResponseSegment{"",
				static_cast<int>(projected.citations.size() + externalIndex + 1)});
			position = end + 1;
		}
	}
  // Phase 25's real structured legal objects and execution facts - direct
  // projections of this same completed operation's own recorded evidence,
  // never a second parse of responseText.
  turn.findings = task.findings;
  turn.obligations = task.obligations;
  turn.events = task.events;
  turn.changes = task.changes;
  // Their words are the lawyer's to read: a source handle in them is rendered
  // against what this operation cited - the answer's references and the
  // entry's own - and one it never issued is named as unverified and counted.
  std::size_t structured_unresolved = 0;
  const auto render = [&](std::string &text, const std::vector<context::ResolvedSourceReference> &own) {
    if (text.find("DOC_") == std::string::npos && text.find("SOURCE") == std::string::npos
        && text.find("CHUNK_") == std::string::npos) return;
    std::vector<context::ResolvedSourceReference> basis = task.source_references;
    basis.insert(basis.end(), own.begin(), own.end());
    context::RenderedSourceHandles rendered = context::render_source_handles(text, basis, context::format_reference_label);
    text = std::move(rendered.text);
    structured_unresolved += rendered.unresolved_count;
  };
  const auto render_optional = [&](std::optional<std::string> &text, const std::vector<context::ResolvedSourceReference> &own) {
    if (text) render(*text, own);
  };
  for (auto &entry : turn.findings) {
    render(entry.title, entry.source_refs);
    render(entry.description, entry.source_refs);
    render_optional(entry.recommendation, entry.source_refs);
  }
  for (auto &entry : turn.obligations) {
    render(entry.obligation, entry.source_refs);
    render_optional(entry.party, entry.source_refs);
    render_optional(entry.trigger, entry.source_refs);
    render_optional(entry.deadline, entry.source_refs);
  }
  for (auto &entry : turn.events) render(entry.description, entry.source_refs);
  for (auto &entry : turn.changes) {
    render(entry.description, entry.source_refs);
    render_optional(entry.category, entry.source_refs);
  }
  // A change written into the document is read the same way: a handle this
  // request never supplied was written as the unverified label, and the turn
  // says so rather than reading as checked.
  structured_unresolved += unverified_labels_written(task);
  if (structured_unresolved > 0) {
    turn.unresolved_reference_count += structured_unresolved;
    turn.source_reference_status = "partial";
  }
  turn.document_change = document_change_from_task(task, project);
  turn.quoted_passages = task.quoted_passages;
  // How many sources this answer stands on, counted from what the operation
  // recorded rather than from how many of them the text happened to point at.
  turn.execution_summary = execution_summary_from_task(task,
      std::max(turn.citations.size(), task.source_references.size()), routing_log_entry);
  turn.professional_review = professional_review_from_task(task);
  turn.result_work_product_ids = task.result_work_product_ids;
  turn.document_review_id = task.document_review_id;
  turn.direct_document_work_title = task.direct_document_work_title;
  turn.direct_document_work_severity = task.direct_document_work_severity;
  turn.direct_document_work_updated = task.direct_document_work_updated;
  turn.direct_document_work_memo = task.direct_document_work_memo;
  turn.work_product_document = task.work_product_document;
  return turn;
}

} // namespace

ChatConversation conversation(const std::string &project_id, const std::string &chat_id) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  const matters::Matter project = require_project(project_id);
  const matters::MatterChat &chat = require_chat(project, chat_id);

  // Safe historical Routing Log metadata (Phase 27's own "Historical outbound
  // payload" rule) - a separate durable per-attempt store, looked up by this
  // exact attempt's operation_id and never by the retry chain's shared
  // task_id, so a retried attempt shows its own latency/routing reason, not
  // an earlier attempt's.
  // One operation can run more than once - the proposal, then the write once
  // the lawyer approves it - and each run is its own Routing Log entry. How
  // long Singularity worked on the turn is all of its runs together; the time the
  // proposal waited for the lawyer is in none of them. The latest run speaks
  // for how the turn was routed.
  std::map<std::string, routing::RoutingLogEntry> routing_log_by_operation_id;
  for (routing::RoutingLogEntry &entry : routing::load_routing_log(workspace_path)) {
    const auto found = routing_log_by_operation_id.find(entry.operation_id);
    if (found == routing_log_by_operation_id.end()) {
      routing_log_by_operation_id.emplace(entry.operation_id, std::move(entry));
      continue;
    }
    const long long worked = found->second.latency_ms + entry.latency_ms;
    if (entry.time >= found->second.time) found->second = std::move(entry);
    found->second.latency_ms = worked;
  }

  ChatConversation result;
  result.project_id = project.id;
  result.chat_id = chat.id;
  result.chat_title = chat.title;
  for (const matters::MatterTask &task : project.tasks) {
    if (!task.chat_id || *task.chat_id != chat_id) continue;
    // Chat shows the operations a person asked for, and in an agent's thread
    // the agent's own words. Automatic Context operations share the same
    // durable collection (MatterTask::operation_type) and are not this
    // Chat's conversation.
    if (task.operation_type != "matter_task" && task.operation_type != "agent_message" && task.operation_type != "member_message") continue;
    if (!finished_operation(task)) continue;
    const auto routing_log_entry = routing_log_by_operation_id.find(task.id);
    result.turns.push_back(turn_from_task(task, project,
        routing_log_entry != routing_log_by_operation_id.end() ? &routing_log_entry->second : nullptr));
  }
  // Each other member's messages on this computer's clock, so a computer whose
  // clock is wrong puts nobody's message out of its place.
  align_member_clocks(result.turns, platform::resolve_actor_identity().actor_id);
  std::stable_sort(result.turns.begin(), result.turns.end(),
      [](const ChatTurn &left, const ChatTurn &right) { return left.created_at < right.created_at; });
  return result;
}

namespace {

// A UTC stamp as Singularity writes one, from seconds since 1970.
std::string utc_stamp(long long seconds) {
  const long long days = (seconds >= 0 ? seconds : seconds - 86399) / 86400;
  const long long rest = seconds - days * 86400;
  const long long shifted = days + 719468;
  const long long era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
  const long long dayOfEra = shifted - era * 146097;
  const long long yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
  const long long dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
  const long long monthIndex = (5 * dayOfYear + 2) / 153;
  const long long day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
  const long long month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
  const long long year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);
  char text[32];
  std::snprintf(text, sizeof text, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ", year, month, day, rest / 3600, rest % 3600 / 60,
      rest % 60);
  return text;
}

void keep_least(std::map<std::string, long long> &kept, const std::string &actor, long long value) {
  const auto [found, isNew] = kept.emplace(actor, value);
  if (isNew == false && value < found->second) found->second = value;
}

}  // namespace

namespace {

// One message as the clocks are read from it: when its writer's clock says it
// was written, whose it is - a member's, or an AI colleague's - what it
// answers and quotes, and when it reached each member.
struct Stamped {
  std::string id;
  std::string created_at;
  bool is_member_message = false;
  std::string author_actor_id;
  std::string agent_id;
  std::string answers_message_id;
  std::string quote_message_id;
  const std::map<std::string, std::string> *received = nullptr;
};

constexpr long long kTwoMinutes = 120;

// Each message's time on this computer's clock, by its index. Every other
// member's messages move by how far that member's clock runs from this one's,
// read from when the messages of the two reached each other. An AI
// colleague's messages stand on the clock of the computer that runs it, which
// carries no received marks of its own: from its answers alone, what cannot
// be is mended - an answer never comes before the message it answers, nor
// after a member's message that replies to it.
std::vector<std::string> on_this_clock(const std::vector<Stamped> &messages, const std::string &viewer) {
  std::vector<std::string> stamps;
  stamps.reserve(messages.size());
  for (const Stamped &message : messages) stamps.push_back(message.created_at);
  if (viewer.empty()) return stamps;
  // For each other member: the least time from this member's message being
  // written to its reaching them - their clock's lead plus the quickest
  // passage -, and the least from theirs being written to its reaching here -
  // the quickest passage less their lead.
  std::map<std::string, long long> mineThere;
  std::map<std::string, long long> theirsHere;
  for (const Stamped &message : messages) {
    if (message.is_member_message == false || message.received == nullptr) continue;
    const std::optional<long long> written = chat::utc_seconds(message.created_at);
    if (written.has_value() == false) continue;
    if (message.author_actor_id.empty() || message.author_actor_id == viewer) {
      for (const auto &[actor, at] : *message.received) {
        if (actor.empty() || actor == viewer) continue;
        if (const std::optional<long long> reached = chat::utc_seconds(at)) keep_least(mineThere, actor, *reached - *written);
      }
      continue;
    }
    const auto here = message.received->find(viewer);
    if (here == message.received->end()) continue;
    if (const std::optional<long long> reached = chat::utc_seconds(here->second))
      keep_least(theirsHere, message.author_actor_id, *reached - *written);
  }
  // How far each one's clock runs ahead of this computer's (behind: less than
  // nought).
  std::map<std::string, long long> lead;
  for (const auto &[actor, least] : theirsHere) {
    const auto there = mineThere.find(actor);
    long long ahead = 0;
    if (there != mineThere.end()) ahead = (there->second - least) / 2;
    // Theirs reached here before it was written: their clock is ahead by at
    // least that much.
    else if (least < 0) ahead = -least;
    if (ahead >= kTwoMinutes || ahead <= -kTwoMinutes) lead[actor] = ahead;
  }
  for (const auto &[actor, least] : mineThere) {
    // Only this member's reached them, before it was written by their clock:
    // theirs is behind by at least that much.
    if (theirsHere.count(actor) != 0 || least > -kTwoMinutes) continue;
    lead[actor] = least;
  }
  // Each message's time in seconds on this clock, a member's moved by its lead.
  std::vector<std::optional<long long>> seconds;
  seconds.reserve(messages.size());
  std::map<std::string, std::size_t> indexOf;
  for (std::size_t index = 0; index < messages.size(); ++index) {
    const Stamped &message = messages[index];
    std::optional<long long> written = chat::utc_seconds(message.created_at);
    if (written.has_value() && message.is_member_message && !message.author_actor_id.empty() && message.author_actor_id != viewer) {
      const auto ahead = lead.find(message.author_actor_id);
      if (ahead != lead.end()) *written -= ahead->second;
    }
    seconds.push_back(written);
    if (!message.id.empty()) indexOf.emplace(message.id, index);
  }
  // A colleague's clock: how far its answers come before what they answer (it
  // runs behind), and how far its messages come after a member's replies to
  // them (it runs ahead); a colleague read both ways is left as it stands.
  std::map<std::string, long long> behind;
  std::map<std::string, long long> aheadBy;
  for (std::size_t index = 0; index < messages.size(); ++index) {
    const Stamped &message = messages[index];
    if (!seconds[index].has_value()) continue;
    if (!message.is_member_message && !message.agent_id.empty() && !message.answers_message_id.empty()) {
      const auto answered = indexOf.find(message.answers_message_id);
      if (answered != indexOf.end() && seconds[answered->second].has_value()) {
        const long long gap = *seconds[index] - *seconds[answered->second];
        if (gap <= -kTwoMinutes) keep_least(behind, message.agent_id, gap);
      }
    }
    if (message.is_member_message && !message.quote_message_id.empty()) {
      const auto quoted = indexOf.find(message.quote_message_id);
      if (quoted == indexOf.end() || !seconds[quoted->second].has_value()) continue;
      const Stamped &colleague = messages[quoted->second];
      if (colleague.is_member_message || colleague.agent_id.empty()) continue;
      const long long gap = *seconds[quoted->second] - *seconds[index];
      if (gap >= kTwoMinutes) {
        const auto [found, isNew] = aheadBy.emplace(colleague.agent_id, gap);
        if (!isNew && gap > found->second) found->second = gap;
      }
    }
  }
  std::map<std::string, long long> colleagueShift;
  for (const auto &[agent, gap] : behind)
    if (aheadBy.count(agent) == 0) colleagueShift[agent] = -gap + 1;
  for (const auto &[agent, gap] : aheadBy)
    if (behind.count(agent) == 0) colleagueShift[agent] = -(gap + 1);
  for (std::size_t index = 0; index < messages.size(); ++index) {
    const Stamped &message = messages[index];
    if (!seconds[index].has_value()) continue;
    long long at = *seconds[index];
    if (!message.is_member_message && !message.agent_id.empty()) {
      const auto shift = colleagueShift.find(message.agent_id);
      if (shift == colleagueShift.end()) continue;
      at += shift->second;
    } else if (!(message.is_member_message && lead.count(message.author_actor_id) != 0)) {
      continue;
    }
    stamps[index] = utc_stamp(at);
  }
  return stamps;
}

}  // namespace

void align_member_clocks(std::vector<ChatTurn> &turns, const std::string &viewer) {
  std::vector<Stamped> messages;
  messages.reserve(turns.size());
  for (const ChatTurn &turn : turns)
    messages.push_back(Stamped{turn.task_id, turn.created_at, turn.is_member_message, turn.author_actor_id, turn.agent_id,
        turn.answers_message_id, turn.quote_message_id, &turn.received});
  const std::vector<std::string> stamps = on_this_clock(messages, viewer);
  for (std::size_t index = 0; index < turns.size(); ++index) turns[index].created_at = stamps[index];
}

namespace {

// Words in one line: their spaces collapsed, at most `most` letters, cut with
// an ellipsis - as a Matter's summary cuts a request (Matter/MatterActions).
std::string in_one_line(const std::string &text, std::size_t most) {
  std::string collapsed;
  bool isSpace = false;
  for (const char character : text) {
    if (character == ' ' || character == '\n' || character == '\t' || character == '\r') {
      isSpace = !collapsed.empty();
      continue;
    }
    if (isSpace) collapsed += ' ';
    isSpace = false;
    collapsed += character;
  }
  std::size_t letters = 0;
  for (std::size_t index = 0; index < collapsed.size(); ++index)
    if ((static_cast<unsigned char>(collapsed[index]) & 0xC0) != 0x80 && ++letters == most)
      return collapsed.substr(0, index) + "…";
  return collapsed;
}

// Whether an operation ended with an answer the thread shows (turn_from_task).
bool answered(const matters::MatterTask &task) {
  return task.response_text.has_value() && !task.stopped_by_user && task.operation_execution_state == "completed"
      && task.status != matters::TaskStatus::Error;
}

// An answer's words as the reader sees them: its segments joined, with no
// citation and no source marker (turn_from_task renders the same segments).
std::string answer_words(const matters::MatterTask &task) {
  const research::ExternalCitationProjection external = research::project_external_citations(
      context::computed_statements::compute(*task.response_text), task.external_evidence);
  const context::ProjectedResponse projected =
      context::project_response_references(external.plain_text, task.source_references);
  std::string words;
  for (const context::ProjectedResponseSegment &segment : projected.segments) {
    if (segment.citation_number > 0) continue;
    std::size_t position = 0;
    while (position < segment.text.size()) {
      const std::size_t start = segment.text.find('\x1e', position);
      if (start == std::string::npos) {
        words += segment.text.substr(position);
        break;
      }
      words += segment.text.substr(position, start - position);
      const std::size_t end = segment.text.find('\x1f', start + 1);
      if (end == std::string::npos) break;
      position = end + 1;
    }
  }
  return words;
}

// Whether a task shows a message of its own in its thread.
bool shows_message(const matters::MatterTask &task) {
  if (!task.chat_id || !task.deleted_at.empty() || !task.thread_event.empty()) return false;
  if (task.operation_type == "member_message" || task.operation_type == "agent_message") return finished_operation(task);
  if (task.operation_type != "matter_task" || !finished_operation(task)) return false;
  // An AI colleague's run shows only its answer; a person's own request shows
  // their words whatever came of it.
  return task.agent_id.empty() || answered(task);
}

} // namespace

std::vector<ChatLatestMessage> latest_messages(const matters::Matter &project) {
  // Each chat's messages, read on this computer's clock as its conversation reads
  // them: the latest is the latest there, and its time is the time there, whatever
  // the clock of the computer that wrote it.
  std::map<std::string, std::vector<const matters::MatterTask *>> byChat;
  for (const matters::MatterTask &task : project.tasks)
    if (shows_message(task)) byChat[*task.chat_id].push_back(&task);
  std::string viewer;
  try {
    viewer = platform::resolve_actor_identity().actor_id;
  } catch (const std::exception &) {
  }
  std::map<std::string, std::pair<const matters::MatterTask *, std::string>> latest;
  for (const auto &[chatId, tasks] : byChat) {
    std::vector<Stamped> messages;
    messages.reserve(tasks.size());
    for (const matters::MatterTask *task : tasks)
      messages.push_back(Stamped{task->id, task->created_at, task->operation_type == "member_message", task->author_actor_id,
          task->agent_id, task->answers_message_id, task->quote_message_id, &task->received});
    const std::vector<std::string> stamps = on_this_clock(messages, viewer);
    std::size_t best = 0;
    for (std::size_t index = 1; index < tasks.size(); ++index) {
      const std::optional<long long> at = chat::utc_seconds(stamps[index]);
      const std::optional<long long> bestAt = chat::utc_seconds(stamps[best]);
      const bool isLater = at.has_value() && bestAt.has_value() ? *bestAt <= *at : stamps[best] <= stamps[index];
      if (isLater) best = index;
    }
    latest.emplace(chatId, std::make_pair(tasks[best], stamps[best]));
  }
  std::vector<ChatLatestMessage> result;
  for (const matters::MatterChat &chat : project.chats) {
    const auto found = latest.find(chat.id);
    if (found == latest.end()) continue;
    const matters::MatterTask &task = *found->second.first;
    ChatLatestMessage message;
    message.chat_id = chat.id;
    message.task_id = task.id;
    message.at = found->second.second;
    message.mentions = task.mentions;
    message.attachment_count = task.attachment_document_ids.size();
    if (task.operation_type == "member_message") {
      message.author_actor_id = task.author_actor_id;
      message.author_name = task.author_name;
      message.text = in_one_line(task.prompt, 160);
    } else if (!task.agent_id.empty()) {
      message.agent_id = task.agent_id;
      message.author_name = task.author_name;
      message.text = in_one_line(answered(task) ? answer_words(task) : task.prompt, 160);
    } else if (answered(task)) {
      // A model's answer to a person's own request, the last of its two.
      message.text = in_one_line(answer_words(task), 160);
    } else {
      message.author_actor_id = task.author_actor_id;
      message.author_name = task.author_name;
      message.text = in_one_line(task.prompt, 160);
    }
    result.push_back(std::move(message));
  }
  return result;
}

ChatSourcePeek resolve_source_reference(const std::string &project_id, const std::string &chat_id,
                                        const std::string &task_id, const std::string &document_id,
                                        const std::string &chunk_id) {
  std::string cited_document_name;
  std::string cited_reading;
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    const matters::Matter project = require_project(project_id);
    (void)require_chat(project, chat_id);

    const auto task = std::find_if(project.tasks.begin(), project.tasks.end(),
        [&](const matters::MatterTask &entry) { return entry.id == task_id; });
    if (task == project.tasks.end() || !task->chat_id || *task->chat_id != chat_id) {
      throw std::runtime_error("This result is no longer part of this Chat.");
    }
    // The citation must be one this exact operation recorded. Without this,
    // a caller could resolve any document of the Project through a Chat
    // result's identity - the reference, not merely the Project, is the
    // boundary here.
    const context::ResolvedSourceReference *reference = recorded_reference(*task, document_id, chunk_id);
    if (reference == nullptr) {
      throw std::runtime_error("This source reference does not belong to this result.");
    }
    cited_document_name = reference->document_name;
    cited_reading = reference->extractor_version;
  }

  // The existing Phase 09 local resolver, unchanged: it re-derives current
  // truth through the same real targeted reconciliation and answers
  // Resolved/Changed/Unavailable. Singularity never searches document text for
  // the citation itself, and never invents a page, heading or excerpt.
  const document::ResolvedSource resolved = document::resolve_source(project_id, document_id, chunk_id, cited_reading);

  ChatSourcePeek peek;
  peek.status = resolved.status == document::SourceResolutionStatus::Resolved ? "resolved"
                : resolved.status == document::SourceResolutionStatus::Changed ? "changed"
                                                                               : "unavailable";
  peek.document_id = resolved.document_id;
  peek.chunk_id = resolved.chunk_id;
  peek.cited_document_name = cited_document_name;
  peek.document_name = resolved.document_name.empty() ? cited_document_name : resolved.document_name;
  peek.current_version = resolved.current_version;
  peek.page = resolved.page;
  peek.heading = resolved.heading;
  peek.text_preview = resolved.text_preview;
  return peek;
}

} // namespace singularity::chat
