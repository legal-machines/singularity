#include "Bridge/ConsiliumBridge/ConsiliumBridge.hpp"

#include "Consilium/ConsiliumActions/ConsiliumActions.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <stdexcept>

namespace singularity::bridge::consilium_bridge {
namespace {

using Json = nlohmann::json;

Json configurationJson(const consilium::ConfigurationView& view) {
	const consilium::Configuration& configuration = view.configuration;
	Json effectiveByParticipant = Json::object();
	for (const consilium::EffectiveParticipantAssignment& assignment : view.effective_assignments) {
		effectiveByParticipant[assignment.participant_id] = Json{
			{"source", assignment.source},
			{"providerId", assignment.model ? Json(assignment.model->provider_id) : Json(nullptr)},
			{"modelId", assignment.model ? Json(assignment.model->model_id) : Json(nullptr)},
		};
	}
	Json participants = Json::array();
	for (const consilium::Participant& participant : configuration.participants) {
		Json explicitModel = nullptr;
		if (participant.explicit_model) {
			explicitModel = Json{{"providerId", participant.explicit_model->provider_id},
				{"modelId", participant.explicit_model->model_id}};
		}
		participants.push_back(Json{
			{"id", participant.id}, {"role", participant.role},
			{"aiSetupRole", participant.ai_setup_role}, {"explicitModel", std::move(explicitModel)},
			{"effectiveAssignment", effectiveByParticipant.at(participant.id)},
		});
	}
	Json stages = Json::array();
	for (const consilium::PlanStage& stage : configuration.stages) {
		stages.push_back(Json{{"id", stage.id}, {"name", stage.name},
			{"mode", consilium::to_string(stage.mode)}, {"participantIds", stage.participant_ids}});
	}
	return Json{
		{"id", configuration.id}, {"projectId", configuration.project_id},
		{"name", configuration.name}, {"objective", configuration.objective},
		{"participants", std::move(participants)}, {"stages", std::move(stages)},
		{"sourcePolicy", Json{{"mode", consilium::to_string(configuration.source_policy.mode)},
			{"selectedDocumentIds", configuration.source_policy.selected_document_ids},
			{"requiresSelection", configuration.source_policy.requires_selection}}},
		{"revision", configuration.revision}, {"createdAt", configuration.created_at},
		{"updatedAt", configuration.updated_at}, {"actorId", configuration.actor_id},
		{"operationId", configuration.operation_id},
	};
}

std::vector<consilium::Participant> participantsFrom(const Json& values) {
	if (values.is_array() == false) throw std::runtime_error("Consilium participants must be a list.");
	std::vector<consilium::Participant> result;
	for (const Json& value : values) {
		if (value.is_object() == false) throw std::runtime_error("A Consilium participant is invalid.");
		consilium::Participant participant;
		participant.id = value.value("id", std::string());
		participant.role = value.value("role", std::string());
		participant.ai_setup_role = value.value("aiSetupRole", std::string());
		if (value.contains("explicitModel") && value.at("explicitModel").is_object()) {
			participant.explicit_model = routing::ModelTarget{
				value.at("explicitModel").value("providerId", std::string()),
				value.at("explicitModel").value("modelId", std::string())};
		}
		result.push_back(std::move(participant));
	}
	return result;
}

std::vector<consilium::PlanStage> stagesFrom(const Json& values) {
	if (values.is_array() == false) throw std::runtime_error("Consilium plan stages must be a list.");
	std::vector<consilium::PlanStage> result;
	for (const Json& value : values) {
		if (value.is_object() == false) throw std::runtime_error("A Consilium plan stage is invalid.");
		const auto mode = consilium::stage_mode_from_string(value.value("mode", std::string()));
		if (mode == std::nullopt) throw std::runtime_error("Choose sequential or parallel for every plan stage.");
		consilium::PlanStage stage;
		stage.id = value.value("id", std::string());
		stage.name = value.value("name", std::string());
		stage.mode = *mode;
		if (value.contains("participantIds") && value.at("participantIds").is_array()) {
			stage.participant_ids = value.at("participantIds").get<std::vector<std::string>>();
		}
		result.push_back(std::move(stage));
	}
	return result;
}

consilium::SourcePolicy sourcePolicyFrom(const Json& value) {
	if (value.is_object() == false) throw std::runtime_error("Choose a valid Consilium source policy.");
	const auto mode = consilium::source_mode_from_string(value.value("mode", std::string()));
	if (mode == std::nullopt) throw std::runtime_error("Choose Relevant Project sources or Selected documents.");
	consilium::SourcePolicy result;
	result.mode = *mode;
	result.requires_selection = value.value("requiresSelection", false);
	if (value.contains("selectedDocumentIds") && value.at("selectedDocumentIds").is_array()) {
		result.selected_document_ids = value.at("selectedDocumentIds").get<std::vector<std::string>>();
	}
	return result;
}

std::string safeFileName(std::string name) {
	for (char& character : name) {
		const unsigned char value = static_cast<unsigned char>(character);
		if (std::isalnum(value) == 0 && character != '-' && character != '_') character = '-';
	}
	while (name.find("--") != std::string::npos) name.replace(name.find("--"), 2, "-");
	if (name.empty()) name = "Consilium";
	return name + ".lm-consilium.json";
}

} // namespace

nlohmann::json list(const nlohmann::json& params) {
	Json configurations = Json::array();
	for (const consilium::ConfigurationView& view :
			consilium::list(params.at("projectId").get<std::string>())) {
		configurations.push_back(configurationJson(view));
	}
	return Json{{"configurations", std::move(configurations)}};
}

nlohmann::json create(const nlohmann::json& params) {
	consilium::CreateConfigurationCommand command;
	command.project_id = params.at("projectId").get<std::string>();
	command.name = params.at("name").get<std::string>();
	command.objective = params.at("objective").get<std::string>();
	return configurationJson(consilium::create(std::move(command)));
}

nlohmann::json update(const nlohmann::json& params) {
	consilium::UpdateConfigurationCommand command;
	command.project_id = params.at("projectId").get<std::string>();
	command.configuration_id = params.at("configurationId").get<std::string>();
	command.expected_revision = params.at("expectedRevision").get<long long>();
	command.name = params.at("name").get<std::string>();
	command.objective = params.at("objective").get<std::string>();
	command.participants = participantsFrom(params.at("participants"));
	command.stages = stagesFrom(params.at("stages"));
	command.source_policy = sourcePolicyFrom(params.at("sourcePolicy"));
	return configurationJson(consilium::update(std::move(command)));
}

nlohmann::json duplicate(const nlohmann::json& params) {
	return configurationJson(consilium::duplicate(
		params.at("projectId").get<std::string>(), params.at("configurationId").get<std::string>()));
}

nlohmann::json import_path(const nlohmann::json& params, const std::string& path) {
	return configurationJson(consilium::import_file(params.at("projectId").get<std::string>(), path));
}

nlohmann::json import_selected(const nlohmann::json& params, const std::vector<std::string>& paths) {
	if (paths.empty()) return Json(nullptr);
	if (paths.size() != 1) throw std::runtime_error("Choose one Consilium configuration file.");
	return import_path(params, paths.front());
}

std::string export_default_name(const nlohmann::json& params) {
	for (const consilium::ConfigurationView& view :
			consilium::list(params.at("projectId").get<std::string>())) {
		if (view.configuration.id == params.at("configurationId").get<std::string>()) {
			return safeFileName(view.configuration.name);
		}
	}
	throw std::runtime_error("This Consilium configuration no longer exists.");
}

nlohmann::json export_path(const nlohmann::json& params, const std::string& path) {
	consilium::export_file(params.at("projectId").get<std::string>(),
		params.at("configurationId").get<std::string>(), path);
	return true;
}

nlohmann::json export_selected(const nlohmann::json& params, const std::optional<std::string>& path) {
	if (path == std::nullopt) return false;
	return export_path(params, *path);
}

} // namespace singularity::bridge::consilium_bridge
