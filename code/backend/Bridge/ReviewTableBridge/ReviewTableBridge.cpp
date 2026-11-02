#include "Bridge/ReviewTableBridge/ReviewTableBridge.hpp"

#include "Platform/DeviceKey/DeviceKey.hpp"
#include "WorkProduct/ReviewTable/ReviewTable.hpp"
#include "WorkProduct/Types/ReviewTablesJson.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace singularity::bridge::review_table_bridge {
namespace {

using Json = nlohmann::json;

std::string required(const Json &params, const char *key) {
	if (!params.contains(key) || !params.at(key).is_string() || params.at(key).get<std::string>().empty())
		throw std::runtime_error(std::string("Missing ") + key + ".");
	return params.at(key).get<std::string>();
}

std::vector<std::string> strings(const Json &value) {
	std::vector<std::string> items;
	if (value.is_array())
		for (const Json &entry : value)
			if (entry.is_string()) items.push_back(entry.get<std::string>());
	return items;
}

Json view_json(const review_table::ReviewTableView &view) {
	Json rows = Json::array();
	for (const matters::ReviewRow &row : view.rows) rows.push_back(matters::review_json::row_to_json(row));
	Json statuses = Json::array();
	for (const review_table::RowStatus &status : view.statuses)
		statuses.push_back(Json{{"documentId", status.document_id}, {"status", status.status},
			{"changeWritten", status.change_written}});
	return Json{{"table", matters::review_json::table_to_json(view.table)}, {"rows", std::move(rows)},
		{"statuses", std::move(statuses)}, {"toAnswer", view.to_answer}, {"inputTokens", view.input_tokens},
		{"outputTokens", view.output_tokens},
		{"costMicros", view.cost_micros ? Json(*view.cost_micros) : Json(nullptr)}, {"costCurrency", view.cost_currency}};
}

} // namespace

Json list(const Json &params) {
	Json tables = Json::array();
	for (const matters::ReviewTable &table : review_table::list(required(params, "projectId")))
		tables.push_back(matters::review_json::table_to_json(table));
	return Json{{"tables", std::move(tables)}};
}

Json get(const Json &params) {
	return view_json(review_table::get(required(params, "projectId"), required(params, "tableId")));
}

Json create(const Json &params) {
	review_table::CreateReviewTable command;
	command.project_id = required(params, "projectId");
	command.title = params.value("title", std::string());
	command.document_ids = strings(params.value("documentIds", Json::array()));
	if (params.contains("columns") && params.at("columns").is_array())
		for (const Json &entry : params.at("columns")) {
			if (!entry.is_object()) continue;
			review_table::NewColumn column;
			column.heading = entry.value("heading", std::string());
			column.question = entry.value("question", std::string());
			column.answer_kind = entry.value("answerKind", std::string("text"));
			column.playbook_id = entry.value("playbookId", std::string());
			column.position_id = entry.value("positionId", std::string());
			column.position = entry.value("position", std::string());
			column.law = entry.value("law", std::string());
			command.columns.push_back(std::move(column));
		}
	command.local_only = params.value("localOnly", false);
	const matters::ReviewTable table = review_table::create(command);
	return view_json(review_table::get(command.project_id, table.id));
}

Json update(const Json &params) {
	review_table::UpdateReviewTable command;
	command.project_id = required(params, "projectId");
	command.table_id = required(params, "tableId");
	if (params.contains("title") && params.at("title").is_string()) command.title = params.at("title").get<std::string>();
	if (params.contains("documentIds") && params.at("documentIds").is_array())
		command.document_ids = strings(params.at("documentIds"));
	if (params.contains("columns") && params.at("columns").is_array()) {
		std::vector<matters::ReviewColumn> columns;
		for (const Json &entry : params.at("columns")) {
			if (!entry.is_object()) continue;
			matters::ReviewColumn column;
			column.id = entry.value("id", std::string());
			column.heading = entry.value("heading", std::string());
			column.question = entry.value("question", std::string());
			column.answer_kind = entry.value("answerKind", std::string("text"));
			column.playbook_id = entry.value("playbookId", std::string());
			column.position_id = entry.value("positionId", std::string());
			column.position = entry.value("position", std::string());
			column.law = entry.value("law", std::string());
			columns.push_back(std::move(column));
		}
		command.columns = std::move(columns);
	}
	if (params.contains("localOnly") && params.at("localOnly").is_boolean()) command.local_only = params.at("localOnly").get<bool>();
	(void)review_table::update(command);
	return view_json(review_table::get(command.project_id, command.table_id));
}

Json remove(const Json &params) {
	return Json{{"removed", review_table::remove(required(params, "projectId"), required(params, "tableId"))}};
}

Json run(const Json &params) {
	review_table::RunOptions options;
	if (params.contains("providerId") && params.at("providerId").is_string() && params.contains("modelId")
			&& params.at("modelId").is_string())
		options.model = routing::ModelTarget{params.at("providerId").get<std::string>(), params.at("modelId").get<std::string>()};
	return view_json(review_table::run(required(params, "projectId"), required(params, "tableId"), options));
}

Json stop(const Json &params) {
	const std::string project = required(params, "projectId");
	const std::string table = required(params, "tableId");
	review_table::request_stop(project, table);
	return view_json(review_table::get(project, table));
}

Json export_table(const Json &params) {
	const std::string project = required(params, "projectId");
	const std::string table = required(params, "tableId");
	const std::string format = required(params, "format");
	const review_table::ReviewTableView view = review_table::get(project, table);
	std::string name = view.table.title;
	for (char &character : name)
		if (character == '/' || character == ':' || character == '\\') character = '-';
	if (format == "xlsx")
		return Json{{"fileName", name + ".xlsx"},
			{"mimeType", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
			{"base64", platform::base64_encode(review_table::export_xlsx(project, table))}};
	if (format == "docx")
		return Json{{"fileName", name + ".docx"},
			{"mimeType", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
			{"base64", platform::base64_encode(review_table::export_docx(project, table))}};
	throw std::runtime_error("A Review Table exports as \"xlsx\" or \"docx\".");
}

} // namespace singularity::bridge::review_table_bridge
