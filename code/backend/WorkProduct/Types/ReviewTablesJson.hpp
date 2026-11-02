#pragma once

#include "WorkProduct/Types/ReviewTables.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace singularity::matters {

// How a Review Table and its rows are stored in the Matter (`reviewTables`,
// `reviewRows`). Read as they stand: a value that does not read is left at
// its default, never guessed.
namespace review_json {

inline std::string text(const nlohmann::json &value, const char *key) {
	return value.contains(key) && value.at(key).is_string() ? value.at(key).get<std::string>() : std::string();
}

inline nlohmann::json evidence_to_json(const ReviewEvidence &evidence) {
	return nlohmann::json{{"quote", evidence.quote}, {"chunkId", evidence.chunk_id}, {"start", evidence.start},
		{"length", evidence.length}, {"page", evidence.page ? nlohmann::json(*evidence.page) : nlohmann::json(nullptr)},
		{"heading", evidence.heading}, {"verified", evidence.verified}, {"aligned", evidence.aligned}};
}

inline ReviewEvidence evidence_from_json(const nlohmann::json &value) {
	ReviewEvidence evidence;
	if (!value.is_object()) return evidence;
	evidence.quote = text(value, "quote");
	evidence.chunk_id = text(value, "chunkId");
	if (value.contains("start") && value.at("start").is_number_unsigned()) evidence.start = value.at("start").get<std::size_t>();
	if (value.contains("length") && value.at("length").is_number_unsigned()) evidence.length = value.at("length").get<std::size_t>();
	if (value.contains("page") && value.at("page").is_number_integer()) evidence.page = value.at("page").get<int>();
	evidence.heading = text(value, "heading");
	evidence.verified = value.contains("verified") && value.at("verified").is_boolean() && value.at("verified").get<bool>();
	evidence.aligned = value.contains("aligned") && value.at("aligned").is_boolean() && value.at("aligned").get<bool>();
	return evidence;
}

inline nlohmann::json cell_to_json(const ReviewCell &cell) {
	nlohmann::json evidence = nlohmann::json::array();
	for (const ReviewEvidence &entry : cell.evidence) evidence.push_back(evidence_to_json(entry));
	return nlohmann::json{{"columnId", cell.column_id}, {"state", cell.state}, {"answer", cell.answer}, {"value", cell.value},
		{"confidence", cell.confidence}, {"basis", cell.basis}, {"reasoning", cell.reasoning},
		{"assessment", cell.assessment}, {"assessmentReason", cell.assessment_reason}, {"evidence", std::move(evidence)}};
}

inline ReviewCell cell_from_json(const nlohmann::json &value) {
	ReviewCell cell;
	if (!value.is_object()) return cell;
	cell.column_id = text(value, "columnId");
	cell.state = text(value, "state");
	cell.answer = text(value, "answer");
	cell.value = text(value, "value");
	cell.confidence = text(value, "confidence");
	cell.basis = text(value, "basis");
	cell.reasoning = text(value, "reasoning");
	cell.assessment = text(value, "assessment");
	cell.assessment_reason = text(value, "assessmentReason");
	if (value.contains("evidence") && value.at("evidence").is_array())
		for (const nlohmann::json &entry : value.at("evidence")) cell.evidence.push_back(evidence_from_json(entry));
	return cell;
}

inline nlohmann::json table_to_json(const ReviewTable &table) {
	nlohmann::json documents = nlohmann::json::array();
	for (const ReviewDocument &document : table.documents)
		documents.push_back(nlohmann::json{{"documentId", document.document_id}, {"name", document.name}});
	nlohmann::json columns = nlohmann::json::array();
	for (const ReviewColumn &column : table.columns)
		columns.push_back(nlohmann::json{{"id", column.id}, {"heading", column.heading}, {"question", column.question},
			{"answerKind", column.answer_kind}, {"playbookId", column.playbook_id}, {"positionId", column.position_id},
			{"position", column.position}, {"law", column.law}});
	return nlohmann::json{{"id", table.id}, {"title", table.title}, {"createdAt", table.created_at},
		{"updatedAt", table.updated_at}, {"createdByActorId", table.created_by_actor_id}, {"documents", std::move(documents)},
		{"columns", std::move(columns)}, {"playbookId", table.playbook_id}, {"playbookVersion", table.playbook_version},
		{"runState", table.run_state}, {"runId", table.run_id}, {"runDeviceId", table.run_device_id},
		{"runStartedAt", table.run_started_at}, {"runFinishedAt", table.run_finished_at},
		{"stopRequested", table.stop_requested}, {"providerId", table.provider_id}, {"modelId", table.model_id},
		{"localOnly", table.local_only}, {"runMessage", table.run_message}, {"workProductId", table.work_product_id},
		{"revision", table.revision}};
}

inline ReviewTable table_from_json(const nlohmann::json &value) {
	ReviewTable table;
	if (!value.is_object()) return table;
	table.id = text(value, "id");
	table.title = text(value, "title");
	table.created_at = text(value, "createdAt");
	table.updated_at = text(value, "updatedAt");
	table.created_by_actor_id = text(value, "createdByActorId");
	if (value.contains("documents") && value.at("documents").is_array())
		for (const nlohmann::json &entry : value.at("documents"))
			if (entry.is_object()) table.documents.push_back(ReviewDocument{text(entry, "documentId"), text(entry, "name")});
	if (value.contains("columns") && value.at("columns").is_array())
		for (const nlohmann::json &entry : value.at("columns")) {
			if (!entry.is_object()) continue;
			ReviewColumn column;
			column.id = text(entry, "id");
			column.heading = text(entry, "heading");
			column.question = text(entry, "question");
			const std::string kind = text(entry, "answerKind");
			if (!kind.empty()) column.answer_kind = kind;
			column.playbook_id = text(entry, "playbookId");
			column.position_id = text(entry, "positionId");
			column.position = text(entry, "position");
			column.law = text(entry, "law");
			table.columns.push_back(std::move(column));
		}
	table.playbook_id = text(value, "playbookId");
	if (value.contains("playbookVersion") && value.at("playbookVersion").is_number_integer())
		table.playbook_version = value.at("playbookVersion").get<int>();
	const std::string state = text(value, "runState");
	if (!state.empty()) table.run_state = state;
	table.run_id = text(value, "runId");
	table.run_device_id = text(value, "runDeviceId");
	table.run_started_at = text(value, "runStartedAt");
	table.run_finished_at = text(value, "runFinishedAt");
	table.stop_requested = value.contains("stopRequested") && value.at("stopRequested").is_boolean()
		&& value.at("stopRequested").get<bool>();
	table.provider_id = text(value, "providerId");
	table.model_id = text(value, "modelId");
	// Anything but a plain false keeps the table's work on this computer.
	table.local_only = value.contains("localOnly") && !(value.at("localOnly").is_boolean() && !value.at("localOnly").get<bool>());
	table.run_message = text(value, "runMessage");
	table.work_product_id = text(value, "workProductId");
	if (value.contains("revision") && value.at("revision").is_number_unsigned())
		table.revision = value.at("revision").get<std::uint64_t>();
	return table;
}

inline nlohmann::json row_to_json(const ReviewRow &row) {
	nlohmann::json cells = nlohmann::json::array();
	for (const ReviewCell &cell : row.cells) cells.push_back(cell_to_json(cell));
	return nlohmann::json{{"id", row.id}, {"tableId", row.table_id}, {"documentId", row.document_id}, {"state", row.state},
		{"message", row.message}, {"documentFingerprint", row.document_fingerprint}, {"documentVersion", row.document_version},
		{"providerId", row.provider_id}, {"modelId", row.model_id}, {"destination", row.destination},
		{"inputTokens", row.input_tokens}, {"outputTokens", row.output_tokens},
		{"costMicros", row.cost_micros ? nlohmann::json(*row.cost_micros) : nlohmann::json(nullptr)},
		{"costCurrency", row.cost_currency}, {"latencyMs", row.latency_ms}, {"answeredAt", row.answered_at},
		{"runId", row.run_id}, {"cells", std::move(cells)}, {"changeOperationId", row.change_operation_id}};
}

inline ReviewRow row_from_json(const nlohmann::json &value) {
	ReviewRow row;
	if (!value.is_object()) return row;
	row.id = text(value, "id");
	row.table_id = text(value, "tableId");
	row.document_id = text(value, "documentId");
	row.state = text(value, "state");
	row.message = text(value, "message");
	row.document_fingerprint = text(value, "documentFingerprint");
	if (value.contains("documentVersion") && value.at("documentVersion").is_number_integer())
		row.document_version = value.at("documentVersion").get<int>();
	row.provider_id = text(value, "providerId");
	row.model_id = text(value, "modelId");
	row.destination = text(value, "destination");
	if (value.contains("inputTokens") && value.at("inputTokens").is_number_unsigned())
		row.input_tokens = value.at("inputTokens").get<std::uint64_t>();
	if (value.contains("outputTokens") && value.at("outputTokens").is_number_unsigned())
		row.output_tokens = value.at("outputTokens").get<std::uint64_t>();
	if (value.contains("costMicros") && value.at("costMicros").is_number_integer())
		row.cost_micros = value.at("costMicros").get<std::int64_t>();
	row.cost_currency = text(value, "costCurrency");
	if (value.contains("latencyMs") && value.at("latencyMs").is_number_integer())
		row.latency_ms = value.at("latencyMs").get<std::int64_t>();
	row.answered_at = text(value, "answeredAt");
	row.run_id = text(value, "runId");
	if (value.contains("cells") && value.at("cells").is_array())
		for (const nlohmann::json &entry : value.at("cells")) row.cells.push_back(cell_from_json(entry));
	row.change_operation_id = text(value, "changeOperationId");
	return row;
}

} // namespace review_json
} // namespace singularity::matters
