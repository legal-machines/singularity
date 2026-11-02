#pragma once

#include "Document/Types/DocumentActionEvidence.hpp"
#include "Document/Types/DocumentLocator.hpp"
#include "Document/Types/DocumentPatchTypes.hpp"

#include <nlohmann/json.hpp>

#include <optional>

namespace singularity::documents {

[[nodiscard]] inline nlohmann::json document_role_assignment_to_json(const DocumentRoleAssignment &assignment) {
	return {{"documentId", assignment.document_id}, {"role", to_string(assignment.role)}};
}

[[nodiscard]] inline std::optional<DocumentRoleAssignment> document_role_assignment_from_json(
		const nlohmann::json &value) {
	if (!value.is_object() || !value.contains("documentId") || !value.at("documentId").is_string())
		return std::nullopt;
	const std::optional<DocumentRole> role = document_role_from_string(value.value("role", std::string()));
	if (!role) return std::nullopt;
	return DocumentRoleAssignment{value.at("documentId").get<std::string>(), *role};
}

[[nodiscard]] inline nlohmann::json document_patch_operation_to_json(const DocumentPatchOperation &operation) {
	nlohmann::json value{{"kind", to_string(operation.kind)},
		{"locator", document_locator_to_json(operation.locator)},
		{"expectedOldValue", operation.expected_old_value}, {"newValue", operation.new_value},
		{"rationaleId", operation.rationale_id}};
	if (operation.placement != DocumentPatchPlacement::After) value["placement"] = to_string(operation.placement);
	if (!operation.cells.empty()) {
		nlohmann::json cells = nlohmann::json::array();
		for (const DocumentPatchCellValue &cell : operation.cells)
			cells.push_back({{"columnKey", cell.column_key}, {"value", cell.value}});
		value["cells"] = std::move(cells);
	}
	if (!operation.formatting.empty()) {
		nlohmann::json formatting = nlohmann::json::object();
		const auto &f = operation.formatting;
		if (f.bold) formatting["bold"] = *f.bold;
		if (f.italic) formatting["italic"] = *f.italic;
		if (f.underline) formatting["underline"] = *f.underline;
		if (f.color_hex) formatting["colorHex"] = *f.color_hex;
		if (f.highlight) formatting["highlight"] = *f.highlight;
		if (f.size_points) formatting["sizePoints"] = *f.size_points;
		if (f.font_name) formatting["fontName"] = *f.font_name;
		if (f.paragraph_style) formatting["paragraphStyle"] = *f.paragraph_style;
		if (f.alignment) formatting["alignment"] = *f.alignment;
		if (f.space_before_points) formatting["spaceBeforePoints"] = *f.space_before_points;
		if (f.space_after_points) formatting["spaceAfterPoints"] = *f.space_after_points;
		value["formatting"] = std::move(formatting);
	}
	return value;
}

// One operation as a model writes it. The shape is the same for every kind:
// what is being acted on (`locator` and, for a passage, the words themselves in
// `expectedOldValue`), what is intended (`kind`), and what the new state is
// (`newValue`, `cells`, or `formatting`). A field a kind does not use may be
// left out; a field written wrongly is a refusal, never a guess.
[[nodiscard]] inline std::optional<DocumentPatchOperation> document_patch_operation_from_json(
		const nlohmann::json &value) {
	if (!value.is_object()) return std::nullopt;
	if (!value.contains("locator")) {
		nlohmann::json withLocator = value;
		withLocator["locator"] = nlohmann::json::object();
		return document_patch_operation_from_json(withLocator);
	}
	// A locator naming nothing (every field null) is an operation that needs no
	// locator - a passage change says where it goes by its own words.
	DocumentPatchOperation operation;
	if (value.at("locator").is_object()) {
		nlohmann::json locatorJson = value.at("locator");
		if (!locatorJson.contains("documentId")) locatorJson["documentId"] = std::string();
		for (const char *key : {"tableId", "rowKey", "columnKey", "heading", "semanticPath"}) {
			if (locatorJson.contains(key) && locatorJson.at(key).is_null()) locatorJson.erase(key);
		}
		const std::optional<DocumentLocator> locator = document_locator_from_json(locatorJson);
		if (!locator.has_value()) return std::nullopt;
		operation.locator = *locator;
	} else if (!value.at("locator").is_null()) {
		return std::nullopt;
	}
	if (value.contains("kind")) {
		if (!value.at("kind").is_string()) return std::nullopt;
		const auto kind = patch_operation_kind_from_string(value.at("kind").get<std::string>());
		if (!kind.has_value()) return std::nullopt;
		operation.kind = *kind;
	} else {
		operation.kind = DocumentPatchOperationKind::ReplaceCellText;
	}

	// A strict schema makes every field present, so "not applicable" arrives as
	// null rather than as an absent key. Both mean the same thing here.
	const auto optionalString = [&](const char *key, std::string &target) {
		if (!value.contains(key) || value.at(key).is_null()) return true;
		if (!value.at(key).is_string()) return false;
		target = value.at(key).get<std::string>();
		return true;
	};
	if (!optionalString("expectedOldValue", operation.expected_old_value)) return std::nullopt;
	if (!optionalString("newValue", operation.new_value)) return std::nullopt;
	if (!optionalString("rationaleId", operation.rationale_id)) return std::nullopt;
	if (!optionalString("newShadingFillHex", operation.new_shading_fill_hex)) return std::nullopt;
	if (!optionalString("newShadingThemeFill", operation.new_shading_theme_fill)) return std::nullopt;
	if (!optionalString("newShadingThemeFillShade", operation.new_shading_theme_fill_shade)) return std::nullopt;

	if (value.contains("placement") && !value.at("placement").is_null()) {
		if (!value.at("placement").is_string()) return std::nullopt;
		const auto placement = patch_placement_from_string(value.at("placement").get<std::string>());
		if (!placement.has_value()) return std::nullopt;
		operation.placement = *placement;
	}

	if (value.contains("cells") && !value.at("cells").is_null()) {
		if (!value.at("cells").is_array()) return std::nullopt;
		for (const nlohmann::json &cell : value.at("cells")) {
			if (!cell.is_object() || !cell.contains("columnKey") || !cell.at("columnKey").is_string()
					|| !cell.contains("value") || !cell.at("value").is_string())
				return std::nullopt;
			operation.cells.push_back({cell.at("columnKey").get<std::string>(), cell.at("value").get<std::string>()});
		}
	}

	if (value.contains("formatting") && !value.at("formatting").is_null()) {
		const nlohmann::json &formatting = value.at("formatting");
		if (!formatting.is_object()) return std::nullopt;
		const auto flag = [&](const char *key, std::optional<bool> &target) {
			if (!formatting.contains(key) || formatting.at(key).is_null()) return true;
			if (!formatting.at(key).is_boolean()) return false;
			target = formatting.at(key).get<bool>();
			return true;
		};
		const auto text = [&](const char *key, std::optional<std::string> &target) {
			if (!formatting.contains(key) || formatting.at(key).is_null()) return true;
			if (!formatting.at(key).is_string()) return false;
			target = formatting.at(key).get<std::string>();
			return true;
		};
		const auto number = [&](const char *key, std::optional<double> &target) {
			if (!formatting.contains(key) || formatting.at(key).is_null()) return true;
			if (!formatting.at(key).is_number()) return false;
			target = formatting.at(key).get<double>();
			return true;
		};
		if (!flag("bold", operation.formatting.bold)) return std::nullopt;
		if (!flag("italic", operation.formatting.italic)) return std::nullopt;
		if (!flag("underline", operation.formatting.underline)) return std::nullopt;
		if (!text("colorHex", operation.formatting.color_hex)) return std::nullopt;
		if (!text("highlight", operation.formatting.highlight)) return std::nullopt;
		if (!number("sizePoints", operation.formatting.size_points)) return std::nullopt;
		if (!text("fontName", operation.formatting.font_name)) return std::nullopt;
		if (!text("paragraphStyle", operation.formatting.paragraph_style)) return std::nullopt;
		if (!text("alignment", operation.formatting.alignment)) return std::nullopt;
		if (!number("spaceBeforePoints", operation.formatting.space_before_points)) return std::nullopt;
		if (!number("spaceAfterPoints", operation.formatting.space_after_points)) return std::nullopt;
	}

	// A cell replacement still requires both of its own values, as it always
	// did: the cell as the model read it, and what it becomes.
	if (operation.kind == DocumentPatchOperationKind::ReplaceCellText) {
		if (!value.contains("expectedOldValue") || !value.contains("newValue")) return std::nullopt;
	}
	// A cell is named by its table, row and column. A "cell" replacement that
	// names none of them, but quotes the words it replaces, is a replacement of
	// those words: models used to tables call every replacement a cell (a live
	// run on a document with no table at all did exactly that). The words still
	// decide - they must stand in the document, once - so nothing is guessed;
	// only the label the model put on the change is read for what it is.
	if (operation.kind == DocumentPatchOperationKind::ReplaceCellText
			&& !operation.locator.table_id && !operation.locator.row_key && !operation.locator.column_key
			&& operation.cells.empty() && !operation.expected_old_value.empty())
		operation.kind = DocumentPatchOperationKind::ReplaceText;
	return operation;
}

[[nodiscard]] inline nlohmann::json document_action_rationale_to_json(const DocumentActionRationale &rationale) {
	using Json = nlohmann::json;
	Json projectEvidence = Json::array();
	for (const DocumentLocator &locator : rationale.project_evidence)
		projectEvidence.push_back(document_locator_to_json(locator));
	Json methodologyEvidence = Json::array();
	for (const DocumentMethodologyEvidence &evidence : rationale.methodology_evidence) {
		methodologyEvidence.push_back({{"documentId", evidence.document_id},
			{"locator", document_locator_to_json(evidence.locator)},
			{"documentFingerprint", evidence.document_fingerprint}, {"note", evidence.note}});
	}
	return {{"id", rationale.id}, {"summary", rationale.summary},
			{"projectEvidence", std::move(projectEvidence)},
			{"methodologyEvidence", std::move(methodologyEvidence)},
			{"externalEvidenceIds", rationale.external_evidence_ids},
			{"externalSupportStatus", to_string(rationale.external_support_status)}};
}

[[nodiscard]] inline std::optional<DocumentActionRationale> document_action_rationale_from_json(
		const nlohmann::json &value) {
	if (!value.is_object() || !value.contains("id") || !value.at("id").is_string()
			|| !value.contains("summary") || !value.at("summary").is_string()) return std::nullopt;
	DocumentActionRationale result;
	result.id = value.at("id").get<std::string>();
	result.summary = value.at("summary").get<std::string>();
	if (value.contains("projectEvidence") && value.at("projectEvidence").is_array()) {
		for (const auto &entry : value.at("projectEvidence")) {
			const auto locator = document_locator_from_json(entry);
			if (!locator) return std::nullopt;
			result.project_evidence.push_back(*locator);
		}
	}
	if (value.contains("methodologyEvidence") && value.at("methodologyEvidence").is_array()) {
		for (const auto &entry : value.at("methodologyEvidence")) {
			if (!entry.is_object() || !entry.contains("locator")) return std::nullopt;
			const auto locator = document_locator_from_json(entry.at("locator"));
			if (!locator) return std::nullopt;
			result.methodology_evidence.push_back({entry.value("documentId", std::string()), *locator,
				entry.value("documentFingerprint", std::string()), entry.value("note", std::string())});
		}
	}
	if (value.contains("externalEvidenceIds") && value.at("externalEvidenceIds").is_array())
		for (const auto &entry : value.at("externalEvidenceIds"))
			if (entry.is_string()) result.external_evidence_ids.push_back(entry.get<std::string>());
	if (value.contains("externalSupportStatus")) {
		if (!value.at("externalSupportStatus").is_string()) return std::nullopt;
		const auto status = external_support_status_from_string(value.at("externalSupportStatus").get<std::string>());
		if (!status) return std::nullopt;
		result.external_support_status = *status;
	}
	return result;
}

[[nodiscard]] inline nlohmann::json document_action_evidence_to_json(
		const AppliedDocumentActionEvidence &evidence) {
	using Json = nlohmann::json;
	Json operations = Json::array();
	for (const DocumentPatchOperation &operation : evidence.operations)
		operations.push_back(document_patch_operation_to_json(operation));
	Json rationales = Json::array();
	for (const DocumentActionRationale &rationale : evidence.rationales)
		rationales.push_back(document_action_rationale_to_json(rationale));
	return Json{{"documentId", evidence.document_id}, {"previousVersion", evidence.previous_version},
		{"previousFingerprint", evidence.previous_fingerprint}, {"resultingVersion", evidence.resulting_version},
		{"resultingFingerprint", evidence.resulting_fingerprint},
		{"toolCallEvidenceId", evidence.tool_call_evidence_id}, {"operations", std::move(operations)},
		{"rationales", std::move(rationales)}};
}

[[nodiscard]] inline std::optional<AppliedDocumentActionEvidence> document_action_evidence_from_json(
		const nlohmann::json &value) {
	if (!value.is_object() || !value.contains("operations") || !value.at("operations").is_array()) return std::nullopt;
	AppliedDocumentActionEvidence evidence;
	evidence.document_id = value.value("documentId", std::string());
	evidence.previous_version = value.value("previousVersion", 0);
	evidence.previous_fingerprint = value.value("previousFingerprint", std::string());
	evidence.resulting_version = value.value("resultingVersion", 0);
	evidence.resulting_fingerprint = value.value("resultingFingerprint", std::string());
	evidence.tool_call_evidence_id = value.value("toolCallEvidenceId", std::string());
	for (const nlohmann::json &entry : value.at("operations")) {
		const std::optional<DocumentPatchOperation> operation = document_patch_operation_from_json(entry);
		if (!operation) return std::nullopt;
		evidence.operations.push_back(*operation);
	}
	if (value.contains("rationales") && value.at("rationales").is_array()) {
		for (const auto &entry : value.at("rationales")) {
			const auto rationale = document_action_rationale_from_json(entry);
			if (!rationale) return std::nullopt;
			evidence.rationales.push_back(*rationale);
		}
	}
	return evidence;
}

} // namespace singularity::documents
