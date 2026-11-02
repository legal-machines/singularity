#include "Bridge/PlaybookBridge/PlaybookBridge.hpp"

#include "WorkProduct/Playbook/Playbook.hpp"
#include "WorkProduct/PlaybookChanges/PlaybookChanges.hpp"
#include "WorkProduct/ReviewTable/ReviewTable.hpp"
#include "WorkProduct/Types/ReviewTablesJson.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace singularity::bridge::playbook_bridge {
namespace {

using Json = nlohmann::json;

std::string required(const Json &params, const char *key) {
	if (!params.contains(key) || !params.at(key).is_string() || params.at(key).get<std::string>().empty())
		throw std::runtime_error(std::string("Missing ") + key + ".");
	return params.at(key).get<std::string>();
}

std::optional<int> version_of(const Json &params) {
	if (params.contains("version") && params.at("version").is_number_integer()) return params.at("version").get<int>();
	return std::nullopt;
}

playbooks::Playbook found(const std::string &id, std::optional<int> version) {
	const std::optional<playbooks::Playbook> playbook = playbooks::get(id, version);
	if (!playbook) throw std::runtime_error("This Playbook is no longer on this computer.");
	return *playbook;
}

} // namespace

Json list(const Json &) {
	Json playbooks = Json::array();
	for (const playbooks::Playbook &playbook : playbooks::list()) playbooks.push_back(playbooks::to_json(playbook));
	return Json{{"playbooks", std::move(playbooks)}};
}

Json get(const Json &params) {
	return playbooks::to_json(found(required(params, "playbookId"), version_of(params)));
}

Json save(const Json &params) {
	if (!params.contains("playbook") || !params.at("playbook").is_object()) throw std::runtime_error("Missing playbook.");
	return playbooks::to_json(playbooks::save(playbooks::from_json(params.at("playbook"))));
}

Json remove(const Json &params) {
	return Json{{"removed", playbooks::remove(required(params, "playbookId"))}};
}

Json import_playbook(const Json &params) {
	return playbooks::to_json(playbooks::import_json(required(params, "text")));
}

Json export_playbook(const Json &params) {
	const playbooks::Playbook playbook = found(required(params, "playbookId"), version_of(params));
	std::string name = playbook.name;
	for (char &character : name)
		if (character == '/' || character == ':' || character == '\\') character = '-';
	return Json{{"fileName", name + " (Playbook v" + std::to_string(playbook.version) + ").json"},
		{"text", playbooks::to_json(playbook).dump(2)}};
}

Json review_table(const Json &params) {
	std::vector<std::string> documents;
	if (params.contains("documentIds") && params.at("documentIds").is_array())
		for (const Json &entry : params.at("documentIds"))
			if (entry.is_string()) documents.push_back(entry.get<std::string>());
	const std::string project = required(params, "projectId");
	const matters::ReviewTable table = playbooks::review_table(project, required(params, "playbookId"), documents,
		params.value("title", std::string()), params.value("localOnly", false));
	const review_table::ReviewTableView view = review_table::get(project, table.id);
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

Json prepare_changes(const Json &params) {
	std::vector<std::string> documents;
	if (params.contains("documentIds") && params.at("documentIds").is_array())
		for (const Json &entry : params.at("documentIds"))
			if (entry.is_string()) documents.push_back(entry.get<std::string>());
	Json changes = Json::array();
	for (const playbook_changes::PreparedChange &change :
			playbook_changes::prepare(required(params, "projectId"), required(params, "tableId"), documents))
		changes.push_back(Json{{"documentId", change.document_id}, {"documentName", change.document_name},
			{"chatId", change.chat_id}, {"operationId", change.operation_id}, {"state", change.state},
			{"message", change.message}, {"columnIds", change.column_ids}});
	return Json{{"changes", std::move(changes)}};
}

Json stop_changes(const Json &params) {
	return Json{{"stopRequested",
		playbook_changes::request_stop(required(params, "projectId"), required(params, "tableId"))}};
}

} // namespace singularity::bridge::playbook_bridge
