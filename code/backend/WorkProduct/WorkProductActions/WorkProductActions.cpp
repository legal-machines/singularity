#include "WorkProduct/WorkProductActions/WorkProductActions.hpp"

#include "Context/ComputedStatements/ComputedStatements.hpp"
#include "Context/SourceLabels/SourceLabels.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "WorkProduct/WorkProductDocument/WorkProductDocument.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>

namespace singularity::work_product {
namespace {

std::string trim(const std::string& value) {
	const std::size_t first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return "";
	return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

std::string noteTitle(const matters::MatterNote& note) {
	if (note.title && note.title->empty() == false) return *note.title;
	const std::size_t lineEnd = note.content.find_first_of("\r\n");
	std::string title = trim(note.content.substr(0, lineEnd));
	if (title.size() > 96) title = title.substr(0, 93) + "...";
	return title.empty() == false ? title : "Note";
}

std::string humanizeIdentifier(const std::string& value) {
	std::string result;
	bool isNewWord = true;
	for (const char character : value) {
		if (character == '-' || character == '_') {
			result += ' ';
			isNewWord = true;
			continue;
		}
		result += isNewWord == true
				? static_cast<char>(std::toupper(static_cast<unsigned char>(character)))
				: character;
		isNewWord = false;
	}
	return result;
}

std::vector<matters::WorkProductType> canonicalTypes() {
	return {
		{matters::kWorkProductTypeNote, "", "Note", "Lightweight retained text.", std::nullopt,
				std::nullopt, true, 1, "", ""},
		{matters::kWorkProductTypeIssue, "", "Issue", "A structured legal issue with a review lifecycle.",
				std::nullopt, std::nullopt, true, 1, "", ""},
		{matters::kWorkProductTypeAnalysis, "", "Analysis", "Retained legal analysis.", std::nullopt,
				std::nullopt, true, 1, "", ""},
		{matters::kWorkProductTypeDraft, "", "Draft", "Retained draft legal work.", std::nullopt,
				std::nullopt, true, 1, "", ""},
		{matters::kWorkProductTypeComparison, "", "Comparison", "Retained comparative legal work.",
				std::nullopt, std::nullopt, true, 1, "", ""},
		{matters::kWorkProductTypeObligationSet, "", "Obligation set", "A retained set of obligations.",
				std::nullopt, std::nullopt, true, 1, "", ""},
		{matters::kWorkProductTypeChronology, "", "Chronology", "A retained legal chronology.", std::nullopt,
				std::nullopt, true, 1, "", ""},
		{matters::kWorkProductTypeReviewTable, "", "Review table",
				"The same questions answered across documents, each answer with the words it rests on.", std::nullopt,
				std::nullopt, true, 1, "", ""},
	};
}

auto findProject(std::vector<matters::Matter>& projects, const std::string& projectId) {
	return std::find_if(projects.begin(), projects.end(), [&](const matters::Matter& project) {
		return project.id == projectId;
	});
}

matters::Matter requireProject(const std::string& projectId) {
	auto found = matters::load_matter(node::require_active_workspace_path(), projectId);
	if (!found) throw std::runtime_error("This Project no longer exists.");
	return *found;
}

const matters::WorkProductType& requireType(
		const std::vector<matters::WorkProductType>& types, const std::string& typeId) {
	const auto found = std::find_if(types.begin(), types.end(), [&](const matters::WorkProductType& type) {
		return type.id == typeId;
	});
	if (found == types.end()) throw std::runtime_error("Choose an available Work Product type.");
	return *found;
}

std::vector<matters::WorkProductType> allTypes(const matters::Matter& project) {
	std::vector<matters::WorkProductType> result = canonicalTypes();
	result.insert(result.end(), project.work_product_types.begin(), project.work_product_types.end());
	return result;
}

matters::WorkProductObject objectFromGeneric(
		const matters::WorkProduct& product, const matters::WorkProductType& type) {
	matters::WorkProductObject result;
	result.id = product.id;
	result.project_id = product.project_id;
	result.type_id = product.type_id;
	result.type_name = type.name;
	result.title = product.title;
	result.content = product.content;
	result.document = product.document;
	result.source_refs = product.source_refs;
	result.findings = product.findings;
	result.obligations = product.obligations;
	result.events = product.events;
	result.changes = product.changes;
	result.origin = product.origin;
	result.revision = product.revision;
	result.created_at = product.created_at;
	result.updated_at = product.updated_at;
	return result;
}

matters::WorkProductObject objectFromNote(const matters::MatterNote& note) {
	matters::WorkProductObject result;
	result.id = note.id;
	result.project_id = note.matter_id;
	result.type_id = matters::kWorkProductTypeNote;
	result.type_name = "Note";
	result.title = noteTitle(note);
	result.content = note.content;
	result.source_refs = note.source_refs;
	result.origin = note.origin;
	result.revision = note.revision;
	result.created_at = note.created_at;
	result.updated_at = note.updated_at.empty() == false ? note.updated_at : note.created_at;
	return result;
}

matters::WorkProductObject objectFromIssue(const matters::Issue& issue) {
	matters::WorkProductObject result;
	result.id = issue.id;
	result.project_id = issue.matter_id;
	result.type_id = matters::kWorkProductTypeIssue;
	result.type_name = "Issue";
	result.title = issue.title;
	result.state = matters::to_string(issue.status);
	result.source_refs = issue.source_refs;
	result.issue = matters::WorkProductIssueData{
		issue.description, matters::to_string(issue.status), issue.source_refs};
	result.origin.kind = issue.origin_finding_id
			? matters::WorkProductOriginKind::Finding
			: matters::WorkProductOriginKind::Legacy;
	result.origin.reference_id = issue.origin_finding_id;
	result.revision = issue.revision;
	result.created_at = issue.created_at;
	result.updated_at = issue.updated_at;
	return result;
}

std::optional<matters::WorkProductObject> objectById(
		const matters::Matter& project, const std::string& workProductId) {
	const auto note = std::find_if(project.notes.begin(), project.notes.end(),
			[&](const matters::MatterNote& value) { return value.id == workProductId; });
	if (note != project.notes.end()) return objectFromNote(*note);
	const auto issue = std::find_if(project.issues.begin(), project.issues.end(),
			[&](const matters::Issue& value) { return value.id == workProductId; });
	if (issue != project.issues.end()) return objectFromIssue(*issue);
	const auto product = std::find_if(project.work_products.begin(), project.work_products.end(),
			[&](const matters::WorkProduct& value) { return value.id == workProductId; });
	if (product == project.work_products.end()) return std::nullopt;
	const std::vector<matters::WorkProductType> types = allTypes(project);
	return objectFromGeneric(*product, requireType(types, product->type_id));
}

matters::WorkProductRevision revisionFromObject(
		const matters::WorkProductObject& object, const std::string& createdAt, bool isCurrent) {
	matters::WorkProductRevision revision;
	revision.id = isCurrent == true
			? "current:" + object.id + ":" + std::to_string(object.revision)
			: matters::generate_work_product_revision_id();
	revision.work_product_id = object.id;
	revision.project_id = object.project_id;
	revision.type_id = object.type_id;
	revision.type_name = object.type_name;
	revision.title = object.title;
	revision.content = object.content;
	revision.state = object.state;
	revision.source_refs = object.source_refs;
	revision.issue = object.issue;
	revision.origin = object.origin;
	revision.revision = object.revision;
	revision.created_at = createdAt;
	revision.is_current = isCurrent;
	return revision;
}

void recordCurrentRevision(matters::Matter& project, const matters::WorkProductObject& object) {
	project.work_product_revisions.push_back(revisionFromObject(object, object.updated_at, false));
}

void requireExpectedRevision(long long currentRevision, long long expectedRevision) {
	if (expectedRevision < 1 || currentRevision != expectedRevision) {
		throw std::runtime_error("This Work Product changed since you opened it. Reload it and try again.");
	}
}

matters::WorkProductProvenance provenanceFor(
		const matters::Matter& project, const matters::WorkProductObject& object) {
	matters::WorkProductProvenance result;
	if (object.origin.kind == matters::WorkProductOriginKind::Manual) {
		result.origin_label = "Created manually";
		return result;
	}
	if (object.origin.kind == matters::WorkProductOriginKind::Legacy) {
		result.origin_label = "Retained before provenance was recorded";
		return result;
	}
	if (object.origin.kind == matters::WorkProductOriginKind::ReviewTable) {
		result.origin_label = "Kept from a Review Table";
		result.is_ai_derived = true;
		return result;
	}
	// An agent's report: what changed was found on this computer; when the
	// check analysed it, the analysis is the operation the reference names.
	if (object.origin.kind == matters::WorkProductOriginKind::Agent) {
		result.origin_label = "Kept by an agent";
		if (!object.origin.reference_id) return result;
		const auto found = std::find_if(project.tasks.begin(), project.tasks.end(),
				[&](const matters::MatterTask& task) { return task.id == *object.origin.reference_id; });
		if (found == project.tasks.end()) return result;
		result.is_ai_derived = true;
		result.operation_state = "available";
		result.operation_id = found->id;
		result.operation_type = found->operation_type;
		result.operation_role = found->operation_role;
		result.operation_status = found->status == matters::TaskStatus::Success ? "success" : "error";
		result.created_at = found->created_at;
		result.provider_id = found->provider_id;
		result.model_id = found->model_id;
		result.execution_kind = found->execution_kind;
		result.chat_state = "not_available";
		if (found->prompt.empty() == false) result.origin_label = found->prompt;
		return result;
	}

	result.is_ai_derived = true;
	const matters::MatterTask* sourceTask = nullptr;
	if (object.origin.kind == matters::WorkProductOriginKind::ChatTask && object.origin.reference_id) {
		const auto found = std::find_if(project.tasks.begin(), project.tasks.end(),
				[&](const matters::MatterTask& task) { return task.id == *object.origin.reference_id; });
		if (found != project.tasks.end()) sourceTask = &*found;
	}
	if (object.origin.kind == matters::WorkProductOriginKind::Finding && object.origin.reference_id) {
		for (const matters::MatterTask& task : project.tasks) {
			const auto finding = std::find_if(task.findings.begin(), task.findings.end(),
					[&](const matters::Finding& value) { return value.id == *object.origin.reference_id; });
			if (finding == task.findings.end()) continue;
			sourceTask = &task;
			break;
		}
	}

	if (sourceTask == nullptr) {
		result.origin_label = object.origin.kind == matters::WorkProductOriginKind::Finding
				? "Source Finding no longer available" : "AI operation no longer available";
		result.operation_state = "unavailable";
		return result;
	}

	result.operation_state = "available";
	result.operation_id = sourceTask->id;
	result.operation_type = sourceTask->operation_type;
	result.operation_role = sourceTask->operation_role;
	result.operation_status = sourceTask->status == matters::TaskStatus::Success ? "success" : "error";
	result.created_at = sourceTask->created_at;
	result.provider_id = sourceTask->provider_id;
	result.model_id = sourceTask->model_id;
	result.execution_kind = sourceTask->execution_kind;

	if (sourceTask->chat_id) {
		result.chat_id = sourceTask->chat_id;
		const auto chat = std::find_if(project.chats.begin(), project.chats.end(),
				[&](const matters::MatterChat& value) { return value.id == *sourceTask->chat_id; });
		if (chat == project.chats.end()) {
			result.chat_state = "deleted";
			result.origin_label = "Source Chat deleted";
		} else {
			result.chat_state = chat->archived == true ? "archived" : "available";
			result.chat_title = chat->title;
			result.origin_label = chat->title;
		}
	} else {
		result.chat_state = "not_available";
	}

	if (sourceTask->workflow_id) result.origin_label = "Document workflow";
	else if (sourceTask->consilium_group_id) result.origin_label = "Consilium";
	else if (result.origin_label.empty() == true) {
		result.origin_label = sourceTask->legal_task_preset_id == "general"
				? "AI result" : humanizeIdentifier(sourceTask->legal_task_preset_id);
	}
	return result;
}

const matters::MatterTask* findTask(const matters::Matter& project, const std::string& taskId) {
	const auto found = std::find_if(project.tasks.begin(), project.tasks.end(),
			[&](const matters::MatterTask& task) { return task.id == taskId; });
	return found != project.tasks.end() ? &*found : nullptr;
}

// The Project document an operation wrote, when it wrote one: the created or
// updated artifact, the register a direct document work changed, or the
// document its applied action names.
std::optional<std::string> writtenDocumentId(const matters::MatterTask& task) {
	if (task.professional_write_intent == false) return std::nullopt;
	if (task.artifact_document_id && task.artifact_document_id->empty() == false) return task.artifact_document_id;
	if (task.direct_document_work_document_id && task.direct_document_work_document_id->empty() == false) {
		return task.direct_document_work_document_id;
	}
	if (task.professional_target_document_id && task.professional_target_document_id->empty() == false) {
		return task.professional_target_document_id;
	}
	if (task.document_action_evidence.empty() == false && task.document_action_evidence.front().document_id.empty() == false) {
		return task.document_action_evidence.front().document_id;
	}
	return std::nullopt;
}

// A kept result the lawyer has worked on since it was kept - edited by hand or
// returned to an earlier version - holds an earlier version recorded while its
// current origin was already in place. Such a result is kept on purpose and is
// never replaced by a later write.
bool isWorkedOnByHand(const matters::Matter& project, const matters::WorkProduct& product) {
	return std::any_of(project.work_product_revisions.begin(), project.work_product_revisions.end(),
			[&](const matters::WorkProductRevision& revision) {
				return revision.work_product_id == product.id && revision.origin.kind == product.origin.kind
						&& revision.origin.reference_id == product.origin.reference_id;
			});
}

// The document a kept result stands for, when it was kept automatically from
// an operation that wrote that document and nobody has worked on it since.
// Several writes of one document are one Highlight: its newest state.
std::optional<std::string> writtenDocumentOf(const matters::Matter& project, const matters::WorkProduct& product) {
	if (product.origin.kind != matters::WorkProductOriginKind::ChatTask || !product.origin.reference_id) return std::nullopt;
	const matters::MatterTask* originTask = findTask(project, *product.origin.reference_id);
	if (originTask == nullptr) return std::nullopt;
	const std::optional<std::string> documentId = writtenDocumentId(*originTask);
	if (documentId == std::nullopt || isWorkedOnByHand(project, product) == true) return std::nullopt;
	return documentId;
}

matters::WorkProductObject createInProject(matters::Matter& project, CreateWorkProductCommand command) {
	command.title = trim(command.title);
	command.content = trim(command.content);
	if (command.title.empty() == true) throw std::runtime_error("Enter a Work Product title.");
	if (command.content.empty() == true) throw std::runtime_error("Enter Work Product content.");

	const std::vector<matters::WorkProductType> types = allTypes(project);
	const matters::WorkProductType& type = requireType(types, command.type_id);
	if (type.id == matters::kWorkProductTypeIssue) {
		throw std::runtime_error("Issues are created from a Finding with Add to Issues.");
	}

	const std::string timestamp = workspaces::current_timestamp();
	if (type.id == matters::kWorkProductTypeNote) {
		matters::MatterNote note;
		note.id = matters::generate_note_id();
		note.matter_id = project.id;
		note.content = command.content;
		note.created_at = timestamp;
		note.updated_at = timestamp;
		note.title = command.title;
		note.source_refs = std::move(command.source_refs);
		note.origin = command.origin;
		project.notes.push_back(note);
		project.updated_at = timestamp;
		return objectFromNote(note);
	}

	matters::WorkProduct product;
	product.id = matters::generate_work_product_id();
	product.project_id = project.id;
	product.type_id = type.id;
	product.title = command.title;
	product.content = command.content;
	product.document = std::move(command.document);
	product.source_refs = std::move(command.source_refs);
	product.findings = std::move(command.findings);
	product.obligations = std::move(command.obligations);
	product.events = std::move(command.events);
	product.changes = std::move(command.changes);
	product.origin = command.origin;
	product.created_at = timestamp;
	product.updated_at = timestamp;
	project.work_products.push_back(product);
	project.updated_at = timestamp;
	return objectFromGeneric(product, type);
}

} // namespace

std::vector<matters::WorkProductType> list_types(const std::string& project_id) {
	return allTypes(requireProject(project_id));
}

std::vector<matters::WorkProductObject> list_objects(const std::string& project_id) {
	const matters::Matter project = requireProject(project_id);
	const std::vector<matters::WorkProductType> types = allTypes(project);

	std::vector<matters::WorkProductObject> result;
	result.reserve(project.notes.size() + project.issues.size() + project.work_products.size());
	for (const matters::MatterNote& note : project.notes) result.push_back(objectFromNote(note));
	for (const matters::Issue& issue : project.issues) result.push_back(objectFromIssue(issue));

	// A document written several times is one Highlight, its newest state.
	// Projects written before writes were kept that way hold one kept result
	// per write; the older ones stay stored (and open by their id) but are not
	// listed twice.
	std::map<std::string, std::string> newestByDocument;
	for (const matters::WorkProduct& product : project.work_products) {
		const std::optional<std::string> documentId = writtenDocumentOf(project, product);
		if (documentId == std::nullopt) continue;
		const std::string key = product.type_id + "\n" + *documentId;
		const auto newest = newestByDocument.find(key);
		if (newest == newestByDocument.end()) {
			newestByDocument[key] = product.id;
			continue;
		}
		const auto current = std::find_if(project.work_products.begin(), project.work_products.end(),
				[&](const matters::WorkProduct& value) { return value.id == newest->second; });
		if (current == project.work_products.end() || current->updated_at < product.updated_at) newest->second = product.id;
	}
	for (const matters::WorkProduct& product : project.work_products) {
		const std::optional<std::string> documentId = writtenDocumentOf(project, product);
		if (documentId != std::nullopt && newestByDocument[product.type_id + "\n" + *documentId] != product.id) continue;
		result.push_back(objectFromGeneric(product, requireType(types, product.type_id)));
	}
	std::sort(result.begin(), result.end(), [](const matters::WorkProductObject& left,
			const matters::WorkProductObject& right) {
		if (left.updated_at != right.updated_at) return left.updated_at > right.updated_at;
		return left.id < right.id;
	});
	return result;
}

matters::WorkProductDetail get_detail(
		const std::string& project_id, const std::string& work_product_id) {
	const matters::Matter project = requireProject(project_id);
	const std::optional<matters::WorkProductObject> object = objectById(project, work_product_id);
	if (object == std::nullopt) throw std::runtime_error("This Work Product no longer exists.");

	matters::WorkProductDetail result;
	result.object = *object;
	const std::vector<matters::WorkProductType> types = allTypes(project);
	for (matters::WorkProductRevision revision : project.work_product_revisions) {
		if (revision.work_product_id != work_product_id) continue;
		revision.type_name = requireType(types, revision.type_id).name;
		result.revisions.push_back(std::move(revision));
	}
	result.revisions.push_back(revisionFromObject(*object, object->updated_at, true));
	std::sort(result.revisions.begin(), result.revisions.end(),
			[](const matters::WorkProductRevision& left, const matters::WorkProductRevision& right) {
				return left.revision > right.revision;
			});
	result.provenance = provenanceFor(project, *object);
	return result;
}

matters::WorkProductType create_type(CreateTypeCommand command) {
	command.name = trim(command.name);
	command.description = trim(command.description);
	if (command.instructions) *command.instructions = trim(*command.instructions);
	if (command.expected_structure) *command.expected_structure = trim(*command.expected_structure);
	if (command.name.empty() == true) throw std::runtime_error("Enter a custom type name.");
	if (command.description.empty() == true) throw std::runtime_error("Describe this custom type.");

	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const std::filesystem::path workspace = node::require_active_workspace_path();
	auto found = matters::load_matter(workspace, command.project_id);
	if (!found) throw std::runtime_error("This Project no longer exists.");

	const std::vector<matters::WorkProductType> existingTypes = canonicalTypes();
	const auto hasName = [&](const matters::WorkProductType& type) {
		return type.name == command.name;
	};
	if (std::any_of(existingTypes.begin(), existingTypes.end(), hasName)
			|| std::any_of(found->work_product_types.begin(), found->work_product_types.end(), hasName)) {
		throw std::runtime_error("A Work Product type with this name already exists.");
	}

	const std::string timestamp = workspaces::current_timestamp();
	matters::WorkProductType type;
	type.id = matters::generate_work_product_type_id();
	type.project_id = found->id;
	type.name = command.name;
	type.description = command.description;
	type.instructions = command.instructions && command.instructions->empty() == false
			? command.instructions : std::nullopt;
	type.expected_structure = command.expected_structure && command.expected_structure->empty() == false
			? command.expected_structure : std::nullopt;
	type.created_at = timestamp;
	type.updated_at = timestamp;
	found->work_product_types.push_back(type);
	found->updated_at = timestamp;
	matters::save_matter(workspace, *found);
	return type;
}

matters::WorkProductObject create(CreateWorkProductCommand command) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const std::filesystem::path workspace = node::require_active_workspace_path();
	auto found = matters::load_matter(workspace, command.project_id);
	if (!found) throw std::runtime_error("This Project no longer exists.");
	matters::WorkProductObject result = createInProject(*found, std::move(command));
	matters::save_matter(workspace, *found);
	return result;
}

namespace {

// A line of a request as a title: its spaces collapsed, at most `most` letters,
// cut with an ellipsis.
std::string title_from_prompt(const std::string &prompt, std::size_t most) {
	std::string collapsed;
	bool isSpace = false;
	for (const char character : prompt) {
		const bool isBlank = character == ' ' || character == '\n' || character == '\t' || character == '\r';
		if (isBlank) {
			isSpace = !collapsed.empty();
			continue;
		}
		if (isSpace) collapsed += ' ';
		isSpace = false;
		collapsed += character;
	}
	std::size_t letters = 0;
	for (std::size_t index = 0; index < collapsed.size(); ++index)
		if ((static_cast<unsigned char>(collapsed[index]) & 0xC0) != 0x80 && ++letters == most) {
			std::string cut = collapsed.substr(0, index);
			while (!cut.empty() && cut.back() == ' ') cut.pop_back();
			return cut + "\u2026";
		}
	return collapsed;
}

// What a finished result is kept as, decided here from the result itself
// (Beta 1 #270): the same answer is kept the same way whoever keeps it - the
// window, the CLI, an AI app or an AI colleague.
std::string retained_type_of(const matters::MatterTask &task, const std::optional<nlohmann::json> &document) {
	if (document) {
		const std::string kind = document->value("kind", std::string());
		if (kind == "draft") return matters::kWorkProductTypeDraft;
		if (kind == "note") return matters::kWorkProductTypeNote;
		return matters::kWorkProductTypeAnalysis;
	}
	const bool wroteDocument = task.direct_document_work_document_id.has_value() || task.artifact_change_kind.has_value()
		|| (task.document_operation_checkpoint && task.document_operation_checkpoint->stage == "committed");
	if (wroteDocument) return matters::kWorkProductTypeDraft;
	if (!task.changes.empty()) return matters::kWorkProductTypeComparison;
	if (!task.obligations.empty()) return matters::kWorkProductTypeObligationSet;
	if (!task.events.empty()) return matters::kWorkProductTypeChronology;
	return matters::kWorkProductTypeAnalysis;
}

std::string retained_title_of(const matters::MatterTask &task, const std::optional<nlohmann::json> &document) {
	if (task.direct_document_work_title && !task.direct_document_work_title->empty())
		return *task.direct_document_work_title;
	if (document) {
		const std::string title = document->value("title", std::string());
		if (!title.empty()) return title;
	}
	return title_from_prompt(task.prompt, 90);
}

} // namespace

matters::WorkProductObject retain_task_result(const std::string& project_id,
		const std::string& task_id, const std::string& type_id, std::string title) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const std::filesystem::path workspace = node::require_active_workspace_path();
	auto found = matters::load_matter(workspace, project_id);
	if (!found) throw std::runtime_error("This Project no longer exists.");
	const auto task = std::find_if(found->tasks.begin(), found->tasks.end(), [&](const matters::MatterTask& value) {
		return value.id == task_id;
	});
	if (task == found->tasks.end()) throw std::runtime_error("This AI result no longer exists.");
	if (task->status != matters::TaskStatus::Success || task->response_text == std::nullopt) {
		throw std::runtime_error("Only a completed AI result can become Work Product.");
	}
	// One AI result is retained once: a second save (a double click, a retry
	// after an ambiguous reply, a save after the automatic retention of a
	// committed change) returns the Work Product that already exists.
	const std::vector<matters::WorkProductType> types = allTypes(*found);
	for (const matters::WorkProduct &product : found->work_products)
		if (product.origin.kind == matters::WorkProductOriginKind::ChatTask && product.origin.reference_id == task_id)
			return objectFromGeneric(product, requireType(types, product.type_id));
	for (const matters::MatterNote &note : found->notes)
		if (note.origin.kind == matters::WorkProductOriginKind::ChatTask && note.origin.reference_id == task_id)
			return objectFromNote(note);
	// A result already linked to a kept one - an earlier write of a document
	// whose Highlight a later write of it carried forward - opens that one.
	for (const std::string &linkedId : task->result_work_product_ids) {
		const std::optional<matters::WorkProductObject> linked = objectById(*found, linkedId);
		if (linked) return *linked;
	}
	std::optional<nlohmann::json> document;
	if (task->work_product_document) {
		const nlohmann::json parsed = nlohmann::json::parse(*task->work_product_document, nullptr, false);
		if (parsed.is_object()) document = parsed;
	}
	CreateWorkProductCommand command;
	command.project_id = project_id;
	// Its kind and its name are the result's own unless the person names them.
	command.type_id = type_id.empty() ? retained_type_of(*task, document) : type_id;
	command.title = title.empty() ? retained_title_of(*task, document) : std::move(title);
	// Retained work is read outside the operation that produced it: internal
	// source handles become the documents' current labels and research markers
	// their sources, never the operation's marker grammar.
	// The universal document when the answer carries one (its Markdown is the
	// retained text, the structure travels with it); otherwise a professional
	// record's complete memo; otherwise the answer itself.
	std::string document_markdown = document ? work_product_document::markdown(*document) : std::string();
	const std::string &retained_text = !document_markdown.empty() ? document_markdown
		: task->direct_document_work_memo && !task->direct_document_work_memo->empty()
			? *task->direct_document_work_memo : *task->response_text;
	if (!document_markdown.empty()) command.document = task->work_product_document;
	if (document && command.title.empty()) command.title = document->value("title", std::string());
	// Kept as the lawyer read it: the arithmetic the answer asked for, worked out.
	command.content = context::source_text_for_people(context::computed_statements::compute(retained_text),
		task->source_references, task->external_evidence, *found);
	command.source_refs = task->source_references;
	// The typed rows travel with the work. A chronology IS its events and an
	// obligation set IS its obligations: leaving them on the task meant the
	// saved work product promised structure it did not carry, and lost it
	// entirely if the task was later retried or deleted.
	command.findings = task->findings;
	command.obligations = task->obligations;
	command.events = task->events;
	command.changes = task->changes;
	command.origin.kind = matters::WorkProductOriginKind::ChatTask;
	command.origin.reference_id = task_id;
	const std::string retained_task_id = task_id;
	const std::string retained_task_created_at = task->created_at;

	// A write of a document already kept from an earlier write of it becomes
	// that Highlight's newest version, not a second Highlight: the earlier
	// state stays in its versions. A result the lawyer has worked on since is
	// never replaced - the new write is kept beside it.
	const std::optional<std::string> documentId = writtenDocumentId(*task);
	auto carried = found->work_products.end();
	if (documentId != std::nullopt) {
		for (auto product = found->work_products.begin(); product != found->work_products.end(); ++product) {
			if (product->type_id != type_id || writtenDocumentOf(*found, *product) != documentId) continue;
			if (carried == found->work_products.end() || carried->updated_at < product->updated_at) carried = product;
		}
	}
	matters::WorkProductObject result;
	if (carried != found->work_products.end()) {
		const matters::MatterTask* keptTask = findTask(*found, carried->origin.reference_id.value_or(std::string()));
		const bool isNewer = keptTask == nullptr || keptTask->created_at <= retained_task_created_at;
		command.title = trim(command.title);
		command.content = trim(command.content);
		const matters::WorkProductType& type = requireType(types, carried->type_id);
		if (isNewer == true && command.content.empty() == false) {
			recordCurrentRevision(*found, objectFromGeneric(*carried, type));
			const std::string timestamp = workspaces::current_timestamp();
			if (command.title.empty() == false) carried->title = command.title;
			carried->content = command.content;
			carried->document = std::move(command.document);
			carried->source_refs = std::move(command.source_refs);
			carried->findings = std::move(command.findings);
			carried->obligations = std::move(command.obligations);
			carried->events = std::move(command.events);
			carried->changes = std::move(command.changes);
			carried->origin = command.origin;
			carried->updated_at = timestamp;
			++carried->revision;
			found->updated_at = timestamp;
		}
		result = objectFromGeneric(*carried, type);
	} else {
		result = createInProject(*found, std::move(command));
	}
	// The result and its Work Product are linked in the same save, so the link
	// can never be lost between two transactions.
	const auto linked = std::find_if(found->tasks.begin(), found->tasks.end(),
		[&](const matters::MatterTask& value) { return value.id == retained_task_id; });
	if (linked != found->tasks.end()
			&& std::find(linked->result_work_product_ids.begin(), linked->result_work_product_ids.end(), result.id)
				== linked->result_work_product_ids.end())
		linked->result_work_product_ids.push_back(result.id);
	matters::save_matter(workspace, *found);
	return result;
}

matters::WorkProductObject update(UpdateWorkProductCommand command) {
	command.title = trim(command.title);
	command.content = trim(command.content);
	if (command.title.empty() == true) throw std::runtime_error("Enter a Work Product title.");
	if (command.content.empty() == true) throw std::runtime_error("Enter Work Product content.");

	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const std::filesystem::path workspace = node::require_active_workspace_path();
	auto project = matters::load_matter(workspace, command.project_id);
	if (!project) throw std::runtime_error("This Project no longer exists.");
	const std::string timestamp = workspaces::current_timestamp();

	const auto note = std::find_if(project->notes.begin(), project->notes.end(),
			[&](const matters::MatterNote& value) { return value.id == command.work_product_id; });
	if (note != project->notes.end()) {
		requireExpectedRevision(note->revision, command.expected_revision);
		if (noteTitle(*note) == command.title && note->content == command.content) return objectFromNote(*note);
		recordCurrentRevision(*project, objectFromNote(*note));
		note->title = command.title;
		note->content = command.content;
		note->updated_at = timestamp;
		++note->revision;
		project->updated_at = timestamp;
		matters::save_matter(workspace, *project);
		return objectFromNote(*note);
	}

	const auto product = std::find_if(project->work_products.begin(), project->work_products.end(),
			[&](const matters::WorkProduct& value) { return value.id == command.work_product_id; });
	if (product == project->work_products.end()) {
		const auto issue = std::find_if(project->issues.begin(), project->issues.end(),
				[&](const matters::Issue& value) { return value.id == command.work_product_id; });
		if (issue != project->issues.end()) {
			throw std::runtime_error("Use the Issue status control to update this structured Issue.");
		}
		throw std::runtime_error("This Work Product no longer exists.");
	}
	requireExpectedRevision(product->revision, command.expected_revision);
	const std::vector<matters::WorkProductType> types = allTypes(*project);
	const matters::WorkProductType& type = requireType(types, product->type_id);
	if (product->title == command.title && product->content == command.content) {
		return objectFromGeneric(*product, type);
	}
	recordCurrentRevision(*project, objectFromGeneric(*product, type));
	if (product->content != command.content) product->document.reset();
	product->title = command.title;
	product->content = command.content;
	product->updated_at = timestamp;
	++product->revision;
	project->updated_at = timestamp;
	matters::save_matter(workspace, *project);
	return objectFromGeneric(*product, type);
}

matters::WorkProductObject update_issue_status(const std::string& project_id,
		const std::string& issue_id, const std::string& status, long long expected_revision) {
	const std::optional<matters::IssueStatus> parsedStatus = matters::issue_status_from_string(status);
	if (parsedStatus == std::nullopt) throw std::runtime_error("Choose a valid Issue status.");
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const std::filesystem::path workspace = node::require_active_workspace_path();
	auto project = matters::load_matter(workspace, project_id);
	if (!project) throw std::runtime_error("This Project no longer exists.");
	const auto issue = std::find_if(project->issues.begin(), project->issues.end(),
			[&](const matters::Issue& value) { return value.id == issue_id; });
	if (issue == project->issues.end()) throw std::runtime_error("This Issue no longer exists.");
	requireExpectedRevision(issue->revision, expected_revision);
	if (issue->status == *parsedStatus) return objectFromIssue(*issue);
	recordCurrentRevision(*project, objectFromIssue(*issue));
	const std::string timestamp = workspaces::current_timestamp();
	issue->status = *parsedStatus;
	issue->updated_at = timestamp;
	++issue->revision;
	project->updated_at = timestamp;
	matters::save_matter(workspace, *project);
	return objectFromIssue(*issue);
}

matters::WorkProductObject restore_revision(const std::string& project_id,
		const std::string& work_product_id, long long revision, long long expected_revision) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const std::filesystem::path workspace = node::require_active_workspace_path();
	auto project = matters::load_matter(workspace, project_id);
	if (!project) throw std::runtime_error("This Project no longer exists.");
	const std::optional<matters::WorkProductObject> current = objectById(*project, work_product_id);
	if (current == std::nullopt) throw std::runtime_error("This Work Product no longer exists.");
	requireExpectedRevision(current->revision, expected_revision);
	if (revision == current->revision) throw std::runtime_error("This is already the current version.");
	const auto targetFound = std::find_if(project->work_product_revisions.begin(), project->work_product_revisions.end(),
			[&](const matters::WorkProductRevision& value) {
				return value.work_product_id == work_product_id && value.revision == revision;
			});
	if (targetFound == project->work_product_revisions.end()) {
		throw std::runtime_error("This Work Product version is no longer available.");
	}
	const matters::WorkProductRevision target = *targetFound;
	if (target.type_id != current->type_id) {
		throw std::runtime_error("This Work Product version does not match the current type.");
	}

	recordCurrentRevision(*project, *current);
	const std::string timestamp = workspaces::current_timestamp();
	const auto note = std::find_if(project->notes.begin(), project->notes.end(),
			[&](const matters::MatterNote& value) { return value.id == work_product_id; });
	if (note != project->notes.end()) {
		if (target.content == std::nullopt) throw std::runtime_error("This Note version has no content.");
		note->title = target.title;
		note->content = *target.content;
		note->source_refs = target.source_refs;
		note->origin = target.origin;
		note->updated_at = timestamp;
		note->revision = current->revision + 1;
		project->updated_at = timestamp;
		matters::save_matter(workspace, *project);
		return objectFromNote(*note);
	}

	const auto issue = std::find_if(project->issues.begin(), project->issues.end(),
			[&](const matters::Issue& value) { return value.id == work_product_id; });
	if (issue != project->issues.end()) {
		if (target.issue == std::nullopt) throw std::runtime_error("This Issue version is incomplete.");
		const std::optional<matters::IssueStatus> status =
				matters::issue_status_from_string(target.issue->status);
		if (status == std::nullopt) throw std::runtime_error("This Issue version has an invalid status.");
		issue->title = target.title;
		issue->description = target.issue->description;
		issue->status = *status;
		issue->source_refs = target.source_refs;
		issue->origin_finding_id = target.origin.kind == matters::WorkProductOriginKind::Finding
				? target.origin.reference_id : std::nullopt;
		issue->updated_at = timestamp;
		issue->revision = current->revision + 1;
		project->updated_at = timestamp;
		matters::save_matter(workspace, *project);
		return objectFromIssue(*issue);
	}

	const auto product = std::find_if(project->work_products.begin(), project->work_products.end(),
			[&](const matters::WorkProduct& value) { return value.id == work_product_id; });
	if (product == project->work_products.end() || target.content == std::nullopt) {
		throw std::runtime_error("This Work Product version is incomplete.");
	}
	product->title = target.title;
	product->content = *target.content;
	product->source_refs = target.source_refs;
	product->origin = target.origin;
	product->updated_at = timestamp;
	product->revision = current->revision + 1;
	const std::vector<matters::WorkProductType> types = allTypes(*project);
	const matters::WorkProductType& type = requireType(types, product->type_id);
	project->updated_at = timestamp;
	matters::save_matter(workspace, *project);
	return objectFromGeneric(*product, type);
}

bool remove(const std::string& project_id, const std::string& work_product_id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const std::filesystem::path workspace = node::require_active_workspace_path();
	auto project = matters::load_matter(workspace, project_id);
	if (!project) throw std::runtime_error("This Project no longer exists.");

	const auto sameId = [&](const auto& value) { return value.id == work_product_id; };
	const auto eraseMatching = [](auto& values, const auto& matches) {
		values.erase(std::remove_if(values.begin(), values.end(), matches), values.end());
	};
	const std::size_t before = project->notes.size() + project->issues.size() + project->work_products.size();
	eraseMatching(project->notes, sameId);
	eraseMatching(project->issues, sameId);
	eraseMatching(project->work_products, sameId);
	if (project->notes.size() + project->issues.size() + project->work_products.size() == before) return false;

	eraseMatching(project->work_product_revisions, [&](const matters::WorkProductRevision& revision) {
		return revision.work_product_id == work_product_id;
	});
	for (matters::MatterTask& task : project->tasks) {
		eraseMatching(task.result_work_product_ids, [&](const std::string& id) { return id == work_product_id; });
	}
	project->updated_at = workspaces::current_timestamp();
	matters::save_matter(workspace, *project);
	return true;
}

} // namespace singularity::work_product
