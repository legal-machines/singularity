#include "Bridge/WorkProductBridge/WorkProductBridge.hpp"

#include "Matter/MatterRepository/MatterRepository.hpp"
#include "WorkProduct/WorkProductActions/WorkProductActions.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace singularity::bridge::work_product_bridge {
namespace {

using Json = nlohmann::json;

Json sourceJson(const context::ResolvedSourceReference& reference) {
	return Json{
		{"documentToken", reference.document_token}, {"chunkToken", reference.chunk_token},
		{"documentId", reference.document_id}, {"chunkId", reference.chunk_id},
		{"documentName", reference.document_name},
		{"page", reference.page ? Json(*reference.page) : Json(nullptr)},
		{"heading", reference.heading ? Json(*reference.heading) : Json(nullptr)},
		// The reading the passage was cut from, so it is found again after Singularity
		// reads that kind of document better.
		{"extractorVersion", reference.extractor_version.empty() ? Json(nullptr) : Json(reference.extractor_version)},
	};
}

Json typeJson(const matters::WorkProductType& type) {
	return Json{
		{"id", type.id}, {"projectId", type.project_id.empty() ? Json(nullptr) : Json(type.project_id)},
		{"name", type.name}, {"description", type.description},
		{"instructions", type.instructions ? Json(*type.instructions) : Json(nullptr)},
		{"expectedStructure", type.expected_structure ? Json(*type.expected_structure) : Json(nullptr)},
		{"isCanonical", type.is_canonical}, {"revision", type.revision},
		{"createdAt", type.created_at.empty() ? Json(nullptr) : Json(type.created_at)},
		{"updatedAt", type.updated_at.empty() ? Json(nullptr) : Json(type.updated_at)},
	};
}

Json objectJson(const matters::WorkProductObject& object) {
	Json sourceRefs = Json::array();
	for (const context::ResolvedSourceReference& reference : object.source_refs) {
		sourceRefs.push_back(sourceJson(reference));
	}
	Json issue = nullptr;
	if (object.issue) {
		Json issueSourceRefs = Json::array();
		for (const context::ResolvedSourceReference& reference : object.issue->source_refs) {
			issueSourceRefs.push_back(sourceJson(reference));
		}
		issue = Json{{"description", object.issue->description}, {"status", object.issue->status},
			{"sourceRefs", std::move(issueSourceRefs)}};
	}
	// The typed rows this work is made of, each with its own sources - so a
	// chronology can be read as its events and an obligation set as its
	// obligations, without a second lookup on the task that produced them.
	const auto rowSources = [&](const std::vector<context::ResolvedSourceReference> &refs) {
		Json result = Json::array();
		for (const context::ResolvedSourceReference &reference : refs) result.push_back(sourceJson(reference));
		return result;
	};
	Json findings = Json::array();
	for (const matters::Finding &finding : object.findings) {
		findings.push_back(Json{{"id", finding.id}, {"title", finding.title},
			{"description", finding.description},
			{"recommendation", finding.recommendation ? Json(*finding.recommendation) : Json(nullptr)},
			{"severity", finding.severity ? Json(*finding.severity) : Json(nullptr)},
			{"sourceRefs", rowSources(finding.source_refs)}});
	}
	Json obligations = Json::array();
	for (const matters::Obligation &obligation : object.obligations) {
		obligations.push_back(Json{{"id", obligation.id},
			{"party", obligation.party ? Json(*obligation.party) : Json(nullptr)},
			{"obligation", obligation.obligation},
			{"trigger", obligation.trigger ? Json(*obligation.trigger) : Json(nullptr)},
			{"deadline", obligation.deadline ? Json(*obligation.deadline) : Json(nullptr)},
			{"sourceRefs", rowSources(obligation.source_refs)}});
	}
	Json events = Json::array();
	for (const matters::Event &event : object.events) {
		events.push_back(Json{{"id", event.id},
			{"date", event.date ? Json(*event.date) : Json(nullptr)},
			{"description", event.description}, {"sourceRefs", rowSources(event.source_refs)}});
	}
	Json changes = Json::array();
	for (const matters::Change &change : object.changes) {
		changes.push_back(Json{{"id", change.id},
			{"category", change.category ? Json(*change.category) : Json(nullptr)},
			{"description", change.description}, {"sourceRefs", rowSources(change.source_refs)}});
	}
	return Json{
		{"id", object.id}, {"projectId", object.project_id}, {"typeId", object.type_id},
		{"typeName", object.type_name}, {"title", object.title},
		{"content", object.content ? Json(*object.content) : Json(nullptr)},
		{"document", object.document ? Json::parse(*object.document, nullptr, false) : Json(nullptr)},
		{"state", object.state ? Json(*object.state) : Json(nullptr)},
		{"sourceRefs", std::move(sourceRefs)},
		{"findings", std::move(findings)}, {"obligations", std::move(obligations)},
		{"events", std::move(events)}, {"changes", std::move(changes)},
		{"issue", std::move(issue)},
		{"origin", Json{{"kind", matters::to_string(object.origin.kind)},
				{"referenceId", object.origin.reference_id ? Json(*object.origin.reference_id) : Json(nullptr)}}},
		{"revision", object.revision}, {"createdAt", object.created_at}, {"updatedAt", object.updated_at},
	};
}

Json revisionJson(const matters::WorkProductRevision& revision) {
	Json sourceRefs = Json::array();
	for (const context::ResolvedSourceReference& reference : revision.source_refs) {
		sourceRefs.push_back(sourceJson(reference));
	}
	Json issue = nullptr;
	if (revision.issue) {
		Json issueSourceRefs = Json::array();
		for (const context::ResolvedSourceReference& reference : revision.issue->source_refs) {
			issueSourceRefs.push_back(sourceJson(reference));
		}
		issue = Json{{"description", revision.issue->description}, {"status", revision.issue->status},
			{"sourceRefs", std::move(issueSourceRefs)}};
	}
	return Json{
		{"id", revision.id}, {"workProductId", revision.work_product_id},
		{"projectId", revision.project_id}, {"typeId", revision.type_id},
		{"typeName", revision.type_name}, {"title", revision.title},
		{"content", revision.content ? Json(*revision.content) : Json(nullptr)},
		{"state", revision.state ? Json(*revision.state) : Json(nullptr)},
		{"sourceRefs", std::move(sourceRefs)}, {"issue", std::move(issue)},
		{"origin", Json{{"kind", matters::to_string(revision.origin.kind)},
			{"referenceId", revision.origin.reference_id ? Json(*revision.origin.reference_id) : Json(nullptr)}}},
		{"revision", revision.revision}, {"createdAt", revision.created_at},
		{"isCurrent", revision.is_current},
	};
}

Json provenanceJson(const matters::WorkProductProvenance& provenance) {
	return Json{
		{"isAiDerived", provenance.is_ai_derived}, {"originLabel", provenance.origin_label},
		{"operationState", provenance.operation_state},
		{"operationId", provenance.operation_id ? Json(*provenance.operation_id) : Json(nullptr)},
		{"operationType", provenance.operation_type}, {"operationRole", provenance.operation_role},
		{"operationStatus", provenance.operation_status}, {"createdAt", provenance.created_at},
		{"chatId", provenance.chat_id ? Json(*provenance.chat_id) : Json(nullptr)},
		{"chatTitle", provenance.chat_title ? Json(*provenance.chat_title) : Json(nullptr)},
		{"chatState", provenance.chat_state},
		{"providerId", provenance.provider_id ? Json(*provenance.provider_id) : Json(nullptr)},
		{"modelId", provenance.model_id ? Json(*provenance.model_id) : Json(nullptr)},
		{"executionKind", provenance.execution_kind},
	};
}

Json detailJson(const matters::WorkProductDetail& detail) {
	Json revisions = Json::array();
	for (const matters::WorkProductRevision& revision : detail.revisions) {
		revisions.push_back(revisionJson(revision));
	}
	return Json{
		{"object", objectJson(detail.object)}, {"revisions", std::move(revisions)},
		{"provenance", provenanceJson(detail.provenance)},
	};
}

} // namespace

nlohmann::json list_types(const nlohmann::json& params) {
	Json result = Json::array();
	for (const matters::WorkProductType& type :
			work_product::list_types(params.at("projectId").get<std::string>())) {
		result.push_back(typeJson(type));
	}
	return Json{{"types", std::move(result)}};
}

nlohmann::json create_type(const nlohmann::json& params) {
	work_product::CreateTypeCommand command;
	command.project_id = params.at("projectId").get<std::string>();
	command.name = params.at("name").get<std::string>();
	command.description = params.at("description").get<std::string>();
	if (params.contains("instructions") && params.at("instructions").is_string()) {
		command.instructions = params.at("instructions").get<std::string>();
	}
	if (params.contains("expectedStructure") && params.at("expectedStructure").is_string()) {
		command.expected_structure = params.at("expectedStructure").get<std::string>();
	}
	return typeJson(work_product::create_type(std::move(command)));
}

nlohmann::json list_objects(const nlohmann::json& params) {
	Json result = Json::array();
	for (const matters::WorkProductObject& object :
			work_product::list_objects(params.at("projectId").get<std::string>())) {
		result.push_back(objectJson(object));
	}
	return Json{{"objects", std::move(result)}};
}

nlohmann::json get_detail(const nlohmann::json& params) {
	return detailJson(work_product::get_detail(
			params.at("projectId").get<std::string>(), params.at("workProductId").get<std::string>()));
}

nlohmann::json create(const nlohmann::json& params) {
	work_product::CreateWorkProductCommand command;
	command.project_id = params.at("projectId").get<std::string>();
	command.type_id = params.at("typeId").get<std::string>();
	command.title = params.at("title").get<std::string>();
	command.content = params.at("content").get<std::string>();
	command.origin.kind = matters::WorkProductOriginKind::Manual;
	return objectJson(work_product::create(std::move(command)));
}

nlohmann::json retain_task_result(const nlohmann::json& params) {
	const auto optional_text = [&](const char *key) {
		return params.contains(key) && params.at(key).is_string() ? params.at(key).get<std::string>() : std::string();
	};
	return objectJson(work_product::retain_task_result(
			params.at("projectId").get<std::string>(), params.at("taskId").get<std::string>(),
			optional_text("typeId"), optional_text("title")));
}

nlohmann::json update(const nlohmann::json& params) {
	work_product::UpdateWorkProductCommand command;
	command.project_id = params.at("projectId").get<std::string>();
	command.work_product_id = params.at("workProductId").get<std::string>();
	command.expected_revision = params.at("expectedRevision").get<long long>();
	command.title = params.at("title").get<std::string>();
	command.content = params.at("content").get<std::string>();
	return objectJson(work_product::update(std::move(command)));
}

nlohmann::json update_issue_status(const nlohmann::json& params) {
	return objectJson(work_product::update_issue_status(
			params.at("projectId").get<std::string>(), params.at("workProductId").get<std::string>(),
			params.at("status").get<std::string>(), params.at("expectedRevision").get<long long>()));
}

nlohmann::json restore_revision(const nlohmann::json& params) {
	return objectJson(work_product::restore_revision(
			params.at("projectId").get<std::string>(), params.at("workProductId").get<std::string>(),
			params.at("revision").get<long long>(), params.at("expectedRevision").get<long long>()));
}

nlohmann::json remove(const nlohmann::json& params) {
	return work_product::remove(
			params.at("projectId").get<std::string>(), params.at("workProductId").get<std::string>());
}

work_product::PreparedExport prepare_export(const nlohmann::json &params) {
  const auto source_type =
      matters::export_source_type_from_string(params.at("sourceType").get<std::string>());
  if (!source_type) throw std::runtime_error("Choose a valid export source.");
  return work_product::prepare_export(
      params.at("matterId").get<std::string>(), params.at("sourceId").get<std::string>(),
      *source_type, params.at("format").get<std::string>());
}

nlohmann::json finish_export(const work_product::PreparedExport &prepared,
                             const std::optional<std::string> &selected_path) {
  if (!selected_path) return false;
  work_product::write_export(*selected_path, prepared);
  return true;
}

} // namespace singularity::bridge::work_product_bridge
