#include "Bridge/ChatExecutionBridge/ChatExecutionBridge.hpp"

#include "Bridge/ChatBridge/ChatBridge.hpp"
#include "Bridge/NativeBridge/NativeError.hpp"
#include "Chat/ChatConversation/ChatConversation.hpp"
#include "Chat/ChatExecution/ChatExecution.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Tool/ToolBroker/ToolBroker.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace singularity::bridge::chat_execution_bridge {
namespace {

using Json = nlohmann::json;

std::string require_operation_id(const Json &params) {
  const auto value = params.at("operationId").get<std::string>();
  if (value.empty()) throw std::runtime_error("This request has no operation reference.");
  return value;
}

Json citation_json(const chat::ChatCitation &citation);

// A change put to the lawyer, as the chat shows it: waiting for a decision,
// or - for a turn that ended in a rejection - the change they declined.
Json approval_json(const tools::ToolApprovalRequest &approval) {
	return Json{
		{"approvalId", approval.approval_id},
		{"summary", approval.summary},
		{"effect", tools::to_string(approval.effect)},
		// Which capability is asking: a change to a document and a search of
		// the public web are different decisions and are shown as such.
		{"toolId", approval.tool_id},
		{"targetDocumentId", approval.target_document_id ? Json(*approval.target_document_id) : Json(nullptr)},
		{"changeKind", approval.change_kind},
		{"removedText", approval.removed_text},
		{"addedText", approval.added_text},
		{"columns", approval.columns},
		{"rows", approval.rows},
		{"change", approval.change_detail_json.empty() ? Json(nullptr)
			: Json::parse(approval.change_detail_json, nullptr, false)},
	};
}

// A question Singularity put to the lawyer in the middle of a run, exactly as they
// were shown it, and what became of it: their answer, or that they left it to
// Singularity, or that it was never answered.
Json lawyer_question_json(const matters::LawyerQuestionRecord &record) {
	return Json{
		{"id", record.id},
		{"title", record.title ? Json(*record.title) : Json(nullptr)},
		{"questions", record.questions.is_array() ? record.questions : Json::array()},
		{"answers", record.answers.is_object() ? record.answers : Json(nullptr)},
		{"state", record.state},
		{"askedAt", record.asked_at},
		{"answeredAt", record.answered_at ? Json(*record.answered_at) : Json(nullptr)},
	};
}

Json status_json(const chat::ChatOperationStatus &status) {
	Json approval = status.approval.has_value() ? approval_json(*status.approval) : Json(nullptr);
	Json events = Json::array();
	for (const matters::AgentOperationEvent &event : status.events) {
		events.push_back(Json{{"id", event.id}, {"sequence", event.sequence}, {"type", event.type},
			{"createdAt", event.created_at}, {"operationRevision", event.operation_revision},
			{"message", event.message},
			{"subjectId", event.subject_id ? Json(*event.subject_id) : Json(nullptr)},
			{"outcome", event.outcome ? Json(*event.outcome) : Json(nullptr)}});
	}
  return Json{
      {"known", status.is_known},
      {"state", chat::to_string(status.state)},
      {"active", status.is_active},
      {"stopRequested", status.is_stop_requested},
      {"runsElsewhere", status.runs_elsewhere},
      {"taskId", status.task_id ? Json(*status.task_id) : Json(nullptr)},
      {"message", status.message.empty() ? Json(nullptr) : Json(status.message)},
	  {"approval", std::move(approval)},
	  {"revision", status.revision},
	  {"turnContractRevision", status.turn_contract_revision},
	  {"stage", status.stage},
	  {"progress", status.progress_stage_key.empty() ? Json(nullptr) : Json{
		  {"stageKey", status.progress_stage_key},
		  {"stageText", status.progress_stage_text},
		  {"elapsedMs", status.elapsed_ms},
		  {"stageElapsedMs", status.stage_elapsed_ms},
		  {"sinceLastEventMs", status.since_last_event_ms},
		  {"candidateDocumentCount", status.candidate_document_count ? Json(*status.candidate_document_count) : Json(nullptr)},
		  {"suppliedDocumentCount", status.supplied_document_count ? Json(*status.supplied_document_count) : Json(nullptr)},
		  {"stages", [&] {
			  Json stages = Json::array();
			  for (const auto &entry : status.stage_history)
				  stages.push_back(Json{{"key", entry.first}, {"startedMs", entry.second}});
			  return stages;
		  }()},
		  {"providerId", status.model_provider_id},
		  {"modelId", status.model_id},
		  {"documentCount", status.document_count},
		  {"external", status.is_external_destination},
	  }},
	  {"resumeAllowed", status.resume_allowed},
	  {"events", std::move(events)},
	  {"citations", [&] {
		  Json citations = Json::array();
		  for (const chat::ChatCitation &citation : status.citations) citations.push_back(citation_json(citation));
		  return citations;
	  }()},
	  // The answer as far as the model has written it, while it writes it.
	  {"liveAnswer", status.live_answer.empty() ? Json(nullptr) : Json(status.live_answer)},
	  // The question the run waits on the lawyer to answer - null when it waits
	  // on none - and every question it has put to them so far.
	  {"asking", status.asking ? *status.asking : Json(nullptr)},
	  {"questions", [&] {
		  Json questions = Json::array();
		  for (const matters::LawyerQuestionRecord &record : status.questions)
			  questions.push_back(lawyer_question_json(record));
		  return questions;
	  }()},
  };
}

Json action_permission_json(const tools::ProjectActionPermissionState &permission) {
	return Json{
		{"requestedMode", tools::to_string(permission.requested_mode)},
		{"effectiveMode", tools::to_string(permission.effective_mode)},
		{"isCapped", permission.is_capped},
		{"capReason", permission.cap_reason.empty() ? Json(nullptr) : Json(permission.cap_reason)},
	};
}

Json source_ref_json(const context::ResolvedSourceReference &reference) {
	return Json{{"documentToken", reference.document_token}, {"chunkToken", reference.chunk_token},
		{"documentId", reference.document_id}, {"chunkId", reference.chunk_id},
		{"documentName", reference.document_name},
		{"page", reference.page ? Json(*reference.page) : Json(nullptr)},
		{"heading", reference.heading ? Json(*reference.heading) : Json(nullptr)}};
}

template <typename T, typename F>
Json array(const std::vector<T> &values, F serialize_value) {
	Json result = Json::array();
	for (const auto &value : values) result.push_back(serialize_value(value));
	return result;
}

// Mirrors Bridge/MatterBridge/MatterBridge.cpp's own finding_json/
// obligation_json/event_json/change_json field-for-field - the same real
// domain objects, the same camelCase mapping, kept local to this Bridge
// file exactly like citation_json above.
Json finding_json(const matters::Finding &finding) {
	return Json{{"id", finding.id}, {"matterId", finding.matter_id}, {"title", finding.title},
		{"description", finding.description},
		{"recommendation", finding.recommendation ? Json(*finding.recommendation) : Json(nullptr)},
		{"severity", finding.severity ? Json(*finding.severity) : Json(nullptr)},
		{"sourceRefs", array(finding.source_refs, source_ref_json)}};
}

Json obligation_json(const matters::Obligation &obligation) {
	return Json{{"id", obligation.id}, {"matterId", obligation.matter_id},
		{"party", obligation.party ? Json(*obligation.party) : Json(nullptr)},
		{"obligation", obligation.obligation},
		{"trigger", obligation.trigger ? Json(*obligation.trigger) : Json(nullptr)},
		{"deadline", obligation.deadline ? Json(*obligation.deadline) : Json(nullptr)},
		{"sourceRefs", array(obligation.source_refs, source_ref_json)}};
}

Json event_json(const matters::Event &event) {
	return Json{{"id", event.id}, {"matterId", event.matter_id},
		{"date", event.date ? Json(*event.date) : Json(nullptr)},
		{"description", event.description}, {"sourceRefs", array(event.source_refs, source_ref_json)}};
}

Json change_json(const matters::Change &change) {
	return Json{{"id", change.id}, {"matterId", change.matter_id},
		{"category", change.category ? Json(*change.category) : Json(nullptr)},
		{"description", change.description}, {"sourceRefs", array(change.source_refs, source_ref_json)}};
}

Json document_change_json(const chat::ChatDocumentChange &change) {
	return Json{{"documentId", change.document_id}, {"resultingVersion", change.resulting_version},
		// Present only for a generic ProfessionalArtifactAction result (Section
		// 15's own UI contract) - null for DirectDocumentWork/AdvancedWorkflow,
		// which keep their own existing specialized presentation fields
		// (directDocumentWorkTitle, professionalReview) instead of these.
		{"artifactTitle", change.artifact_title ? Json(*change.artifact_title) : Json(nullptr)},
		{"artifactFormat", change.artifact_format ? Json(*change.artifact_format) : Json(nullptr)},
		{"artifactChangeKind", change.artifact_change_kind ? Json(*change.artifact_change_kind) : Json(nullptr)},
		// The change as written - the chat shows it the way the document now does.
		{"change", change.artifact_change ? Json::parse(*change.artifact_change, nullptr, false) : Json(nullptr)}};
}

Json execution_summary_json(const chat::ChatExecutionSummary &summary) {
	return Json{
		{"primaryRole", summary.primary_role},
		{"primaryProviderId", summary.primary_provider_id ? Json(*summary.primary_provider_id) : Json(nullptr)},
		{"primaryModelId", summary.primary_model_id ? Json(*summary.primary_model_id) : Json(nullptr)},
		{"executionLocation", summary.execution_location},
		{"protectionApplied", summary.protection_applied},
		{"sourceReferenceCount", summary.source_reference_count},
		{"inputTokens", summary.input_tokens ? Json(*summary.input_tokens) : Json(nullptr)},
		{"outputTokens", summary.output_tokens ? Json(*summary.output_tokens) : Json(nullptr)},
		{"totalTokens", summary.total_tokens ? Json(*summary.total_tokens) : Json(nullptr)},
		{"costMicros", summary.cost_micros ? Json(*summary.cost_micros) : Json(nullptr)},
		{"costCurrency", summary.cost_currency ? Json(*summary.cost_currency) : Json(nullptr)},
		{"latencyMs", summary.latency_ms ? Json(*summary.latency_ms) : Json(nullptr)},
		{"routingReason", summary.routing_reason ? Json(*summary.routing_reason) : Json(nullptr)},
	};
}

Json evidence_objective_json(const chat::ChatEvidenceObjective &objective) {
	Json ranges = Json::array();
	for (const chat::ChatEvidenceCoverageRange &range : objective.inspected_ranges) {
		ranges.push_back(Json{{"startRow", range.start_row}, {"endRow", range.end_row}});
	}
	return Json{{"id", objective.id}, {"description", objective.description},
		{"tableHandle", objective.table_handle}, {"coverageMode", objective.coverage_mode},
		{"inspectedRanges", std::move(ranges)}, {"closed", objective.closed}};
}

Json professional_review_json(const chat::ChatProfessionalReview &review) {
	Json issues = Json::array();
	for (const chat::ChatProfessionalReviewIssue &issue : review.issues) {
		issues.push_back(Json{{"category", issue.category}, {"message", issue.message},
			{"suggestedNextAction", issue.suggested_next_action}});
	}
	return Json{{"verdict", review.verdict}, {"reviewedRevision", review.reviewed_revision},
		{"issues", std::move(issues)},
		{"evidenceObjectives", array(review.evidence_objectives, evidence_objective_json)}};
}

Json citation_json(const chat::ChatCitation &citation) {
  return Json{
      {"number", citation.number},
	  {"sourceKind", citation.source_kind},
      {"documentId", citation.document_id},
      {"chunkId", citation.chunk_id},
      {"documentName", citation.document_name},
      {"page", citation.page ? Json(*citation.page) : Json(nullptr)},
      {"heading", citation.heading ? Json(*citation.heading) : Json(nullptr)},
      {"label", citation.label},
	  {"evidenceId", citation.evidence_id ? Json(*citation.evidence_id) : Json(nullptr)},
	  {"url", citation.url ? Json(*citation.url) : Json(nullptr)},
	  {"domain", citation.domain ? Json(*citation.domain) : Json(nullptr)},
	  {"sourceClass", citation.source_class ? Json(*citation.source_class) : Json(nullptr)},
	  {"excerpt", citation.excerpt},
  };
}

Json unavailable_source_document_json(const matters::UnavailableSourceDocument &entry) {
  return Json{{"documentId", entry.document_id}, {"documentName", entry.document_name}, {"reason", entry.reason}};
}

// A message's reactions: each emoji, and who put it, in the order kept.
Json reactions_json(const std::map<std::string, std::vector<std::string>> &reactions) {
  Json list = Json::array();
  for (const auto &[emoji, who] : reactions) list.push_back(Json{{"emoji", emoji}, {"actorIds", who}});
  return list;
}

// When a person's message reached each other member (D-212): actor id to time.
Json received_json(const std::map<std::string, std::string> &received) {
  Json found = Json::object();
  for (const auto &[actor, at] : received) found[actor] = at;
  return found;
}

// The place in a document a message is about, or null.
Json place_json(const matters::DocumentPlace &place) {
  if (place.empty()) return Json(nullptr);
  return Json{{"documentId", place.document_id}, {"section", place.section}, {"heading", place.heading},
      {"quote", place.quote}};
}

Json turn_json(const chat::ChatTurn &turn) {
  Json segments = Json::array();
  for (const chat::ChatResponseSegment &segment : turn.response_segments) {
    segments.push_back(Json{{"text", segment.text}, {"citationNumber", segment.citation_number}});
  }
  Json citations = Json::array();
  for (const chat::ChatCitation &citation : turn.citations) citations.push_back(citation_json(citation));
  return Json{
      {"taskId", turn.task_id},
      {"createdAt", turn.created_at},
      {"prompt", turn.prompt},
      {"agentId", turn.agent_id},
      {"isAgentMessage", turn.is_agent_message},
      {"agentInitiated", turn.agent_initiated},
      {"isMemberMessage", turn.is_member_message},
      {"messageProvenance", turn.message_provenance},
      {"authorActorId", turn.author_actor_id},
      {"authorName", turn.author_name},
      {"mentions", turn.mentions},
      {"answersMessageId", turn.answers_message_id},
      {"quoteMessageId", turn.quote_message_id},
      {"attachmentDocumentIds", turn.attachment_document_ids},
      {"reactions", reactions_json(turn.reactions)},
      {"received", received_json(turn.received)},
      {"editedAt", turn.edited_at},
      {"deletedAt", turn.deleted_at},
      {"notChecked", turn.not_checked},
      {"askOptions", turn.ask_options},
      {"answerOption", turn.answer_option},
      {"threadEvent", turn.thread_event},
      {"place", place_json(turn.place)},
      {"canChange", turn.can_change},
      {"status", turn.status},
      {"responseSegments", std::move(segments)},
      {"citations", std::move(citations)},
      {"unresolvedReferenceCount", turn.unresolved_reference_count},
      {"sourceReferenceStatus", turn.source_reference_status},
      {"legalTaskPresetId", turn.legal_task_preset_id},
      {"legalTaskPresetVersion", turn.legal_task_preset_version},
      {"sourcePolicyMode", turn.source_policy_mode},
      {"selectedDocumentIds", turn.selected_document_ids},
      {"providerId", turn.provider_id ? Json(*turn.provider_id) : Json(nullptr)},
      {"modelId", turn.model_id ? Json(*turn.model_id) : Json(nullptr)},
      {"executionKind", turn.execution_kind},
      {"protectedApiMode", turn.protected_api_mode},
      {"protectedApiApplied", turn.protected_api_applied},
	  {"researchMode", turn.research_mode},
      {"responseIntegrity", turn.response_integrity},
      {"unresolvedFigureNotice", turn.unresolved_figure_notice},
      {"protectedValueCount", turn.protected_value_count},
      {"protectedCategoryCounts", turn.protected_category_counts},
      {"error", turn.error ? Json(*turn.error) : Json(nullptr)},
      {"errorCode", turn.error_code ? Json(*turn.error_code) : Json(nullptr)},
      {"findings", array(turn.findings, finding_json)},
      {"obligations", array(turn.obligations, obligation_json)},
      {"events", array(turn.events, event_json)},
      {"changes", array(turn.changes, change_json)},
      {"documentChange", turn.document_change ? document_change_json(*turn.document_change) : Json(nullptr)},
      {"rejectedChange", turn.rejected_change ? approval_json(*turn.rejected_change) : Json(nullptr)},
      {"questions", array(turn.questions, lawyer_question_json)},
      // Checked quotations: what the answer quoted, whether those words stand
      // in the passage it cited, and where they stand so the document opens
      // at them.
      {"quotedPassages", [&] {
        Json quoted = Json::array();
        for (const context::QuotedPassage &passage : turn.quoted_passages)
          quoted.push_back(Json{{"quote", passage.quote}, {"documentId", passage.document_id},
              {"chunkId", passage.chunk_id}, {"documentName", passage.document_name},
              {"verified", passage.verified}, {"start", static_cast<long long>(passage.start)},
              {"length", static_cast<long long>(passage.length)}, {"attributed", passage.attributed}});
        return quoted;
      }()},
      {"executionSummary",
          turn.execution_summary ? execution_summary_json(*turn.execution_summary) : Json(nullptr)},
      {"professionalReview",
          turn.professional_review ? professional_review_json(*turn.professional_review) : Json(nullptr)},
      {"resultWorkProductIds", turn.result_work_product_ids},
      {"documentReviewId", turn.document_review_id.empty() ? Json(nullptr) : Json(turn.document_review_id)},
      {"directDocumentWorkTitle",
          turn.direct_document_work_title ? Json(*turn.direct_document_work_title) : Json(nullptr)},
      {"workProductDocument", turn.work_product_document
          ? Json::parse(*turn.work_product_document, nullptr, false) : Json(nullptr)},
      {"directDocumentWork", turn.direct_document_work_title ? Json{
          {"title", *turn.direct_document_work_title},
          {"severity", turn.direct_document_work_severity ? Json(*turn.direct_document_work_severity) : Json(nullptr)},
          {"kind", turn.direct_document_work_updated.value_or(false) ? "updated" : "added"},
          {"memo", turn.direct_document_work_memo ? Json(*turn.direct_document_work_memo) : Json(nullptr)},
      } : Json(nullptr)},
      {"consiliumGroupId", turn.consilium_group_id ? Json(*turn.consilium_group_id) : Json(nullptr)},
      {"consiliumRole", turn.consilium_role ? Json(*turn.consilium_role) : Json(nullptr)},
      {"workflowRunId", turn.workflow_run_id ? Json(*turn.workflow_run_id) : Json(nullptr)},
      {"workflowId", turn.workflow_id ? Json(*turn.workflow_id) : Json(nullptr)},
      {"workflowVersion", turn.workflow_version ? Json(*turn.workflow_version) : Json(nullptr)},
      {"workflowStepId", turn.workflow_step_id ? Json(*turn.workflow_step_id) : Json(nullptr)},
      {"workflowName", turn.workflow_name ? Json(*turn.workflow_name) : Json(nullptr)},
      {"workflowStepName", turn.workflow_step_name ? Json(*turn.workflow_step_name) : Json(nullptr)},
      {"contextBudgetOmittedMaterial", turn.context_budget_omitted_material},
      {"requestCoverage", [&] {
          Json sources = Json::array();
          for (const matters::SourceCoverage &source : turn.request_coverage.sources)
            sources.push_back(Json{{"documentId", source.document_id}, {"supplied", source.supplied},
                {"retrieved", source.retrieved}, {"explicitlySelected", source.explicitly_selected}});
          return Json{
            {"contextItems", turn.request_coverage.context_items},
            {"omittedContextItemIds", turn.request_coverage.omitted_context_item_ids},
            {"conversationTurns", turn.request_coverage.conversation_turns},
            {"turnsInFull", turn.request_coverage.turns_in_full},
            {"turnsAsRecords", turn.request_coverage.turns_as_records},
            {"omittedTurns", turn.request_coverage.omitted_turns},
            {"sources", std::move(sources)},
            {"relevanceSelection", turn.request_coverage.relevance_selection},
            {"relevanceCandidateDocuments", turn.request_coverage.relevance_candidate_documents}};
        }()},
      {"unavailableSourceDocuments", array(turn.unavailable_source_documents, unavailable_source_document_json)},
  };
}

// The code a failed or conflicting operation recorded on its task. A request
// the provider refused does not throw: its attempt records the code on the
// task and the operation ends Failed. Reading it back is only for saying the
// failure - if the Project cannot be read now, the failure is still said, as
// one Singularity could not name.
std::optional<providers::TaskErrorCode> recorded_error_code(const std::string &project_id,
		const std::string &task_id) {
	if (task_id.empty()) return std::nullopt;
	try {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const auto project = matters::load_matter(node::require_active_workspace_path(), project_id);
		if (!project) return std::nullopt;
		const auto found = std::find_if(project->tasks.begin(), project->tasks.end(),
			[&](const matters::MatterTask &task) { return task.id == task_id; });
		return found == project->tasks.end() ? std::nullopt : found->error_code;
	} catch (...) {
		return std::nullopt;
	}
}

// A request's outcome as the interface is given it. A failure - thrown, or
// recorded by the operation itself - carries its stable code and the sentence
// for the lawyer; `message` keeps what was recorded, word for word, for
// Details only.
Json outcome_json(const std::string &project_id, const chat::ChatExecutionOutcome &outcome,
		std::optional<NativeError> failure) {
	const bool failed = outcome.state == chat::ChatOperationState::Failed
		|| outcome.state == chat::ChatOperationState::Conflict;
	if (failed && !failure)
		failure = recordedFailureOf(recorded_error_code(project_id, outcome.task_id),
			outcome.state == chat::ChatOperationState::Conflict, outcome.message);
	return Json{
		{"operationId", outcome.operation_id},
		{"state", chat::to_string(outcome.state)},
		{"taskId", outcome.task_id.empty() ? Json(nullptr) : Json(outcome.task_id)},
		{"message", outcome.message.empty() ? Json(nullptr) : Json(outcome.message)},
		{"errorCode", failure ? Json(failure->code) : Json(nullptr)},
		{"userMessage", failure ? Json(failure->userMessage) : Json(nullptr)},
		{"approval", Json(nullptr)},
	};
}

} // namespace

nlohmann::json send(const nlohmann::json &params) {
  // A send always continues one Chat: without one there is no conversation to
  // continue and no boundary keeping other Chats out of it. A preview may name
  // none, so the shared parse below reads it as optional, and the send asks
  // for it first.
  const auto chat_id = params.find("chatId");
  if (chat_id == params.end() || !chat_id->is_string() || chat_id->get<std::string>().empty())
    throw std::runtime_error("Choose a Chat before sending.");
  // Exactly the same validated parse Phase 22's setup resolution and Phase
  // 23's preview use - a one-time model override can never reach execution
  // through a second, unvalidated path, and an unknown source-policy mode
  // can never be silently defaulted into a real send.
  const chat::ChatRequestComposition composition = chat_bridge::chat_request_composition(params);
  const std::string operationId = require_operation_id(params);
  chat::ChatExecutionOutcome outcome;
  std::optional<NativeError> failure;
  try {
    outcome = chat::run_chat_request(operationId, composition);
  } catch (...) {
    // The operation has already recorded its own failure (run_chat_operation
    // persists it before rethrowing); what the app is given is that failed
    // operation, its stable code and the sentence that says why for a lawyer,
    // never a rejected call. `message` keeps what was recorded, word for word.
    failure = nativeErrorOf(std::current_exception());
    outcome.operation_id = operationId;
    outcome.state = chat::ChatOperationState::Failed;
    outcome.message = failure->recorded;
  }
  return outcome_json(composition.project_id, outcome, std::move(failure));
}

nlohmann::json resume_operation(const nlohmann::json &params) {
	const std::string projectId = params.at("projectId").get<std::string>();
	const chat::ChatExecutionOutcome outcome = chat::resume_chat_operation(projectId, require_operation_id(params));
	return outcome_json(projectId, outcome, std::nullopt);
}

nlohmann::json list_operations(const nlohmann::json &params) {
	Json operations = Json::array();
	for (const chat::ChatOperationSummary &summary :
			chat::list_operations(params.at("projectId").get<std::string>())) {
		operations.push_back(Json{{"operationId", summary.operation_id}, {"taskId", summary.task_id},
			{"chatId", summary.chat_id ? Json(*summary.chat_id) : Json(nullptr)},
			{"prompt", summary.prompt}, {"state", chat::to_string(summary.state)},
			{"stage", summary.stage}, {"activity", summary.activity},
			{"updatedAt", summary.updated_at}, {"resumeAllowed", summary.resume_allowed},
			{"waitingForApproval", summary.waiting_for_approval},
			{"changeCommitted", summary.change_committed}, {"runsElsewhere", summary.runs_elsewhere}});
	}
	return Json{{"operations", std::move(operations)}};
}

nlohmann::json stop_operation(const nlohmann::json &params) {
  return status_json(chat::request_stop(params.at("projectId").get<std::string>(),
                                        require_operation_id(params)));
}

nlohmann::json operation_state(const nlohmann::json &params) {
  return status_json(chat::operation_status(params.at("projectId").get<std::string>(),
                                            require_operation_id(params)));
}

nlohmann::json action_permission(const nlohmann::json &params) {
	return action_permission_json(tools::project_action_permission(
		params.at("projectId").get<std::string>()));
}

nlohmann::json save_action_permission(const nlohmann::json &params) {
	const std::optional<tools::ProjectActionMode> requestedMode =
		tools::project_action_mode_from_string(params.at("requestedMode").get<std::string>());
	if (requestedMode.has_value() == false) {
		throw std::runtime_error("Choose a supported Project action permission.");
	}
	return action_permission_json(tools::save_project_action_permission(
		params.at("projectId").get<std::string>(), *requestedMode));
}

nlohmann::json respond_tool_approval(const nlohmann::json &params) {
	const std::string decision = params.at("decision").get<std::string>();
	if (decision != "allow_once" && decision != "allow_for_request" && decision != "deny") {
		throw std::runtime_error("Choose whether to allow this action once or deny it.");
	}
	const std::string projectId = params.at("projectId").get<std::string>();
	const std::string operationId = require_operation_id(params);
	const std::string approvalId = params.at("approvalId").get<std::string>();
	// ChatExecution owns the decision: an operation still executing receives it
	// through its in-process wait, a paused one records it durably against the
	// exact approval its checkpoint names (surviving a restart).
	// "allow_for_request" allows web research for the rest of this request only.
	return status_json(chat::record_approval_response(projectId, operationId, approvalId, decision != "deny",
		decision == "allow_for_request"));
}

nlohmann::json answer_question(const nlohmann::json &params) {
	// Only the answer's shape is read here: {questionId: {optionIds, text}}, or
	// null when the lawyer leaves the decisions to Singularity. Which questions and
	// options it may name is the question's own to say (chat::
	// record_question_answer), and a refused answer leaves the question open.
	const Json &given = params.at("answers");
	const auto readable = [](const Json &answer) {
		return answer.is_object() && answer.contains("optionIds") && answer.at("optionIds").is_array()
			&& std::all_of(answer.at("optionIds").begin(), answer.at("optionIds").end(),
				[](const Json &optionId) { return optionId.is_string(); })
			&& answer.contains("text") && answer.at("text").is_string();
	};
	if (!given.is_null() && (!given.is_object() || !std::all_of(given.begin(), given.end(), readable)))
		throw std::runtime_error("This answer could not be read. Answer the question again.");
	const std::string projectId = params.at("projectId").get<std::string>();
	const std::string operationId = require_operation_id(params);
	const std::string questionId = params.at("questionId").get<std::string>();
	// ChatExecution owns the answer: the attempt that asked waits for it in this
	// process and continues with it at once - there is nothing to resume.
	return status_json(chat::record_question_answer(projectId, operationId, questionId,
		given.is_null() ? std::nullopt : std::optional<Json>(given)));
}

nlohmann::json conversation(const nlohmann::json &params) {
  const chat::ChatConversation result = chat::conversation(params.at("projectId").get<std::string>(),
                                                           params.at("chatId").get<std::string>());
  Json turns = Json::array();
  for (const chat::ChatTurn &turn : result.turns) turns.push_back(turn_json(turn));
  return Json{
      {"projectId", result.project_id},
      {"chatId", result.chat_id},
      {"chatTitle", result.chat_title},
      {"turns", std::move(turns)},
  };
}

nlohmann::json resolve_source_reference(const nlohmann::json &params) {
  const chat::ChatSourcePeek peek = chat::resolve_source_reference(
      params.at("projectId").get<std::string>(), params.at("chatId").get<std::string>(),
      params.at("taskId").get<std::string>(), params.at("documentId").get<std::string>(),
      params.at("chunkId").get<std::string>());
  return Json{
      {"status", peek.status},
      {"documentId", peek.document_id},
      {"chunkId", peek.chunk_id},
      {"citedDocumentName", peek.cited_document_name},
      {"documentName", peek.document_name},
      {"currentVersion", peek.current_version},
      {"page", peek.page ? Json(*peek.page) : Json(nullptr)},
      {"heading", peek.heading ? Json(*peek.heading) : Json(nullptr)},
      {"textPreview", peek.text_preview},
  };
}

} // namespace singularity::bridge::chat_execution_bridge
