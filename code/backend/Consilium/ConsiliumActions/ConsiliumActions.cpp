#include "Consilium/ConsiliumActions/ConsiliumActions.hpp"

#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AppSettings/AppSettings.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

namespace singularity::consilium {
namespace {

using Json = nlohmann::json;

std::string trim(const std::string& value) {
	const std::size_t first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return "";
	return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

auto findProject(std::vector<matters::Matter>& projects, const std::string& projectId) {
	return std::find_if(projects.begin(), projects.end(), [&](const matters::Matter& project) {
		return project.id == projectId;
	});
}

routing::AiSetupOverrideSet nodeDefaultOverrides() {
	routing::AiSetupOverrideSet result;
	for (const platform::RoleModelAssignment& assignment :
			platform::load_app_settings().local_node_setup.role_assignments) {
		const auto role = routing::ai_setup_role_from_string(assignment.role);
		if (role) {
			result.push_back(routing::RoleAssignment{
				*role, routing::ModelTarget{assignment.provider_id, assignment.model_id}});
		}
	}
	return result;
}

routing::AiSetupOverrideSet projectOverrides(const matters::Matter& project) {
	routing::AiSetupOverrideSet result;
	for (const matters::AiSetupRoleOverride& assignment : project.ai_setup_overrides) {
		const auto role = routing::ai_setup_role_from_string(assignment.role);
		if (role) {
			result.push_back(routing::RoleAssignment{
				*role, routing::ModelTarget{assignment.provider_id, assignment.model_id}});
		}
	}
	return result;
}

ConfigurationView viewFor(const matters::Matter& project, const Configuration& configuration) {
	ConfigurationView result;
	result.configuration = configuration;
	const routing::AiSetupOverrideSet nodeDefaults = nodeDefaultOverrides();
	const routing::AiSetupOverrideSet projectSetup = projectOverrides(project);
	for (const Participant& participant : configuration.participants) {
		const auto role = routing::ai_setup_role_from_string(participant.ai_setup_role);
		if (role == std::nullopt) throw std::runtime_error("This Consilium participant has an invalid AI Setup role.");
		routing::AiSetupOverrideSet participantOverride;
		if (participant.explicit_model) {
			participantOverride.push_back(routing::RoleAssignment{*role, *participant.explicit_model});
		}
		const routing::ResolvedRoleAssignment resolved = routing::resolve_role(
			*role, nodeDefaults, projectSetup, {}, participantOverride);
		result.effective_assignments.push_back(EffectiveParticipantAssignment{
			participant.id, resolved.model,
			participant.explicit_model ? "consiliumOverride" : routing::to_string(resolved.source)});
	}
	return result;
}

void requireUniqueName(const matters::Matter& project, const std::string& name,
		const std::string& exceptId = "") {
	const auto found = std::find_if(project.consilium_configurations.begin(),
		project.consilium_configurations.end(), [&](const Configuration& configuration) {
			return configuration.id != exceptId && configuration.name == name;
		});
	if (found != project.consilium_configurations.end()) {
		throw std::runtime_error("A Consilium configuration with this name already exists.");
	}
}

void validateSourcePolicy(const matters::Matter& project, const SourcePolicy& policy) {
	if (policy.mode == SourceMode::RelevantProject) {
		if (policy.selected_document_ids.empty() == false) {
			throw std::runtime_error("Relevant Project sources cannot also contain selected documents.");
		}
		if (policy.requires_selection) {
			throw std::runtime_error("Relevant Project sources do not require a document selection.");
		}
		return;
	}
	std::set<std::string> seen;
	for (const std::string& documentId : policy.selected_document_ids) {
		if (seen.insert(documentId).second == false) {
			throw std::runtime_error("A selected source is listed more than once.");
		}
		const auto found = std::find_if(project.documents.begin(), project.documents.end(),
			[&](const matters::MatterDocument& document) { return document.id == documentId; });
		if (found == project.documents.end()) {
			throw std::runtime_error("A selected Consilium document is no longer in this Project.");
		}
	}
	if (policy.requires_selection != policy.selected_document_ids.empty()) {
		throw std::runtime_error(policy.selected_document_ids.empty()
			? "Choose at least one document for Selected documents."
			: "Selected Project documents no longer require source selection.");
	}
}

void validateConfiguration(const matters::Matter& project, Configuration& configuration) {
	configuration.name = trim(configuration.name);
	configuration.objective = trim(configuration.objective);
	if (configuration.name.empty()) throw std::runtime_error("Enter a Consilium name.");
	if (configuration.objective.empty()) throw std::runtime_error("Describe what the Consilium should review.");
	if (configuration.participants.empty()) throw std::runtime_error("Add at least one Consilium participant.");

	std::set<std::string> participantIds;
	for (Participant& participant : configuration.participants) {
		participant.role = trim(participant.role);
		if (participant.id.empty() || participant.role.empty()) {
			throw std::runtime_error("Every Consilium participant needs a stable identity and role.");
		}
		if (participantIds.insert(participant.id).second == false) {
			throw std::runtime_error("A Consilium participant is listed more than once.");
		}
		const auto seatRole = routing::ai_setup_role_from_string(participant.ai_setup_role);
		if (seatRole == std::nullopt) {
			throw std::runtime_error("Choose a valid AI Setup role for every participant.");
		}
		// A seat has to be able to write an answer. Anonymization is Papinian's
		// own role and Semantic search is retrieval; neither produces a position
		// a panel can deliberate over, so seating a participant on one of them
		// builds a panel that can only refuse.
		if (*seatRole == routing::AiSetupRole::Anonymization || *seatRole == routing::AiSetupRole::SemanticSearch) {
			throw std::runtime_error(std::string("A Consilium participant cannot sit on the ")
				+ routing::display_label(*seatRole) + " role: it does not write answers. Choose Reasoning, "
				"Drafting or Context.");
		}
		if (participant.explicit_model
				&& (participant.explicit_model->provider_id.empty() || participant.explicit_model->model_id.empty())) {
			throw std::runtime_error("Choose a complete model assignment or use the Project setup.");
		}
	}

	if (configuration.stages.empty()) throw std::runtime_error("Add at least one Consilium plan stage.");
	std::set<std::string> stageIds;
	std::set<std::string> plannedParticipants;
	for (PlanStage& stage : configuration.stages) {
		stage.name = trim(stage.name);
		if (stage.id.empty() || stage.name.empty() || stage.participant_ids.empty()) {
			throw std::runtime_error("Every plan stage needs a stable identity, name and participant.");
		}
		if (stageIds.insert(stage.id).second == false) {
			throw std::runtime_error("A Consilium plan stage is listed more than once.");
		}
		for (const std::string& participantId : stage.participant_ids) {
			if (participantIds.count(participantId) == 0) {
				throw std::runtime_error("A plan stage refers to a participant who is no longer on the team.");
			}
			if (plannedParticipants.insert(participantId).second == false) {
				throw std::runtime_error("Each participant can appear only once in this reusable plan.");
			}
		}
	}
	if (plannedParticipants != participantIds) {
		throw std::runtime_error("Every participant needs a place in the Consilium plan.");
	}
	validateSourcePolicy(project, configuration.source_policy);
}

void replaceDraftIdentities(Configuration& configuration) {
	std::map<std::string, std::string> participantIds;
	for (Participant& participant : configuration.participants) {
		if (participant.id.rfind("draft:", 0) != 0) continue;
		const std::string draftId = participant.id;
		participant.id = matters::generate_consilium_participant_id();
		participantIds.emplace(draftId, participant.id);
	}
	for (PlanStage& stage : configuration.stages) {
		if (stage.id.rfind("draft:", 0) == 0) stage.id = matters::generate_consilium_stage_id();
		for (std::string& participantId : stage.participant_ids) {
			const auto replacement = participantIds.find(participantId);
			if (replacement != participantIds.end()) participantId = replacement->second;
		}
	}
}

Configuration defaultConfiguration(const matters::Matter& project, CreateConfigurationCommand command) {
	Configuration configuration;
	configuration.id = matters::generate_consilium_configuration_id();
	configuration.project_id = project.id;
	configuration.name = std::move(command.name);
	configuration.objective = std::move(command.objective);
	// Every seat on the panel a Consilium creates by itself is a seat that can
	// actually speak. The default used to seat Research on Semantic search and
	// Synthesis on Drafting: a node with only Reasoning assigned - which is the
	// ordinary case - then produced a panel that refused to run the moment it
	// was used, truthfully but uselessly. The seats keep their professional
	// names and the three-stage plan is unchanged; a lawyer who has assigned
	// Drafting can still move Synthesis to it in the editor.
	configuration.participants = {
		{matters::generate_consilium_participant_id(), "Research", "reasoning", std::nullopt},
		{matters::generate_consilium_participant_id(), "Review", "reasoning", std::nullopt},
		{matters::generate_consilium_participant_id(), "Challenge", "reasoning", std::nullopt},
		{matters::generate_consilium_participant_id(), "Synthesis", "reasoning", std::nullopt},
	};
	configuration.stages = {
		{matters::generate_consilium_stage_id(), "Research documents", StageMode::Sequential,
			{configuration.participants[0].id}},
		{matters::generate_consilium_stage_id(), "Independent review", StageMode::Parallel,
			{configuration.participants[1].id, configuration.participants[2].id}},
		{matters::generate_consilium_stage_id(), "Synthesis", StageMode::Sequential,
			{configuration.participants[3].id}},
	};
	return configuration;
}

void stamp(Configuration& configuration, bool isCreate) {
	const std::string timestamp = workspaces::current_timestamp();
	if (isCreate) configuration.created_at = timestamp;
	configuration.updated_at = timestamp;
	configuration.actor_id = platform::resolve_actor_identity().actor_id;
	configuration.operation_id = matters::generate_consilium_operation_id();
}

void requireOnlyKeys(const Json& value, const std::set<std::string>& allowed,
		const std::string& objectName) {
	if (value.is_object() == false) throw std::runtime_error(objectName + " must be an object.");
	for (const auto& entry : value.items()) {
		if (allowed.count(entry.key()) == 0) {
			throw std::runtime_error("The Consilium file contains an unsupported " + objectName + " field.");
		}
	}
}

Json portableJson(const Configuration& configuration) {
	Json participants = Json::array();
	for (const Participant& participant : configuration.participants) {
		Json explicitModel = nullptr;
		if (participant.explicit_model) {
			explicitModel = Json{{"providerId", participant.explicit_model->provider_id},
				{"modelId", participant.explicit_model->model_id}};
		}
		participants.push_back(Json{{"id", participant.id}, {"role", participant.role},
			{"aiSetupRole", participant.ai_setup_role}, {"explicitModel", std::move(explicitModel)}});
	}
	Json stages = Json::array();
	for (const PlanStage& stage : configuration.stages) {
		stages.push_back(Json{{"id", stage.id}, {"name", stage.name},
			{"mode", to_string(stage.mode)}, {"participantIds", stage.participant_ids}});
	}
	return Json{
		{"format", "singularity-consilium"}, {"schemaVersion", 1},
		{"configuration", Json{{"name", configuration.name}, {"objective", configuration.objective},
			{"participants", std::move(participants)}, {"stages", std::move(stages)},
			{"sourcePolicy", Json{{"mode", to_string(configuration.source_policy.mode)},
				{"requiresSelection", configuration.source_policy.mode == SourceMode::SelectedDocuments}}}}},
	};
}

Configuration configurationFromPortable(const Json& root, const std::string& projectId) {
	requireOnlyKeys(root, {"format", "schemaVersion", "configuration"}, "file");
	if (root.value("format", std::string()) != "singularity-consilium") {
		throw std::runtime_error("Choose an Singularity Consilium configuration file.");
	}
	if (root.value("schemaVersion", 0) != 1) {
		throw std::runtime_error("This Consilium configuration uses an incompatible version.");
	}
	if (root.contains("configuration") == false) throw std::runtime_error("The Consilium file has no configuration.");
	const Json& source = root.at("configuration");
	requireOnlyKeys(source, {"name", "objective", "participants", "stages", "sourcePolicy"}, "configuration");
	if (source.contains("participants") == false || source.at("participants").is_array() == false
			|| source.contains("stages") == false || source.at("stages").is_array() == false) {
		throw std::runtime_error("The Consilium configuration has an invalid team or plan.");
	}

	Configuration result;
	result.id = matters::generate_consilium_configuration_id();
	result.project_id = projectId;
	result.name = source.value("name", std::string());
	result.objective = source.value("objective", std::string());
	std::map<std::string, std::string> participantIds;
	for (const Json& entry : source.at("participants")) {
		requireOnlyKeys(entry, {"id", "role", "aiSetupRole", "explicitModel"}, "participant");
		const std::string exportedId = entry.value("id", std::string());
		if (exportedId.empty() || participantIds.count(exportedId)) {
			throw std::runtime_error("The Consilium file contains an invalid participant identity.");
		}
		Participant participant;
		participant.id = matters::generate_consilium_participant_id();
		participant.role = entry.value("role", std::string());
		participant.ai_setup_role = entry.value("aiSetupRole", std::string());
		if (entry.contains("explicitModel") && entry.at("explicitModel").is_object()) {
			const Json& model = entry.at("explicitModel");
			requireOnlyKeys(model, {"providerId", "modelId"}, "model assignment");
			participant.explicit_model = routing::ModelTarget{
				model.value("providerId", std::string()), model.value("modelId", std::string())};
		} else if (entry.contains("explicitModel") && entry.at("explicitModel").is_null() == false) {
			throw std::runtime_error("The Consilium file contains an invalid model assignment.");
		}
		participantIds.emplace(exportedId, participant.id);
		result.participants.push_back(std::move(participant));
	}
	for (const Json& entry : source.at("stages")) {
		requireOnlyKeys(entry, {"id", "name", "mode", "participantIds"}, "plan stage");
		const auto mode = stage_mode_from_string(entry.value("mode", std::string()));
		if (mode == std::nullopt || entry.contains("participantIds") == false
				|| entry.at("participantIds").is_array() == false) {
			throw std::runtime_error("The Consilium file contains an invalid plan stage.");
		}
		PlanStage stage;
		stage.id = matters::generate_consilium_stage_id();
		stage.name = entry.value("name", std::string());
		stage.mode = *mode;
		for (const Json& exportedParticipantId : entry.at("participantIds")) {
			if (exportedParticipantId.is_string() == false
					|| participantIds.count(exportedParticipantId.get<std::string>()) == 0) {
				throw std::runtime_error("A Consilium plan stage refers to an unknown participant.");
			}
			stage.participant_ids.push_back(participantIds.at(exportedParticipantId.get<std::string>()));
		}
		result.stages.push_back(std::move(stage));
	}
	if (source.contains("sourcePolicy") == false) throw std::runtime_error("The Consilium file has no source policy.");
	const Json& policy = source.at("sourcePolicy");
	requireOnlyKeys(policy, {"mode", "requiresSelection"}, "source policy");
	const auto sourceMode = source_mode_from_string(policy.value("mode", std::string()));
	if (sourceMode == std::nullopt) throw std::runtime_error("The Consilium file has an invalid source policy.");
	if (policy.contains("requiresSelection") == false || policy.at("requiresSelection").is_boolean() == false
			|| policy.at("requiresSelection").get<bool>() != (*sourceMode == SourceMode::SelectedDocuments)) {
		throw std::runtime_error("The Consilium file has an invalid portable source policy.");
	}
	result.source_policy.mode = *sourceMode;
	result.source_policy.requires_selection = policy.at("requiresSelection").get<bool>();
	return result;
}

std::string duplicateName(const matters::Matter& project, const std::string& sourceName) {
	std::string candidate = sourceName + " copy";
	int suffix = 2;
	while (std::any_of(project.consilium_configurations.begin(), project.consilium_configurations.end(),
			[&](const Configuration& configuration) { return configuration.name == candidate; })) {
		candidate = sourceName + " copy " + std::to_string(suffix++);
	}
	return candidate;
}

} // namespace

std::vector<ConfigurationView> list(const std::string& projectId) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const std::vector<matters::Matter> projects = matters::load_matters(node::require_active_workspace_path());
	const auto project = std::find_if(projects.begin(), projects.end(),
		[&](const matters::Matter& value) { return value.id == projectId; });
	if (project == projects.end()) throw std::runtime_error("This Project no longer exists.");
	std::vector<ConfigurationView> result;
	for (const Configuration& configuration : project->consilium_configurations) {
		result.push_back(viewFor(*project, configuration));
	}
	return result;
}

ConfigurationView create(CreateConfigurationCommand command) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const std::filesystem::path workspace = node::require_active_workspace_path();
	auto project = matters::load_matter(workspace, command.project_id);
	if (!project) throw std::runtime_error("This Project no longer exists.");
	Configuration configuration = defaultConfiguration(*project, std::move(command));
	validateConfiguration(*project, configuration);
	requireUniqueName(*project, configuration.name);
	stamp(configuration, true);
	project->consilium_configurations.push_back(configuration);
	project->updated_at = configuration.updated_at;
	matters::save_matter(workspace, *project);
	return viewFor(*project, configuration);
}

ConfigurationView update(UpdateConfigurationCommand command) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const std::filesystem::path workspace = node::require_active_workspace_path();
	auto project = matters::load_matter(workspace, command.project_id);
	if (!project) throw std::runtime_error("This Project no longer exists.");
	const auto found = std::find_if(project->consilium_configurations.begin(),
		project->consilium_configurations.end(), [&](const Configuration& value) {
			return value.id == command.configuration_id;
		});
	if (found == project->consilium_configurations.end()) {
		throw std::runtime_error("This Consilium configuration no longer exists.");
	}
	if (command.expected_revision < 1 || command.expected_revision != found->revision) {
		throw std::runtime_error("This Consilium configuration changed since you opened it. Reload it and try again.");
	}
	Configuration next = *found;
	next.name = std::move(command.name);
	next.objective = std::move(command.objective);
	next.participants = std::move(command.participants);
	next.stages = std::move(command.stages);
	next.source_policy = std::move(command.source_policy);
	replaceDraftIdentities(next);
	validateConfiguration(*project, next);
	requireUniqueName(*project, next.name, next.id);
	++next.revision;
	stamp(next, false);
	*found = next;
	project->updated_at = next.updated_at;
	matters::save_matter(workspace, *project);
	return viewFor(*project, next);
}

ConfigurationView duplicate(const std::string& projectId, const std::string& configurationId) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const std::filesystem::path workspace = node::require_active_workspace_path();
	auto project = matters::load_matter(workspace, projectId);
	if (!project) throw std::runtime_error("This Project no longer exists.");
	const auto source = std::find_if(project->consilium_configurations.begin(),
		project->consilium_configurations.end(), [&](const Configuration& value) {
			return value.id == configurationId;
		});
	if (source == project->consilium_configurations.end()) {
		throw std::runtime_error("This Consilium configuration no longer exists.");
	}
	Configuration copy = *source;
	copy.id = matters::generate_consilium_configuration_id();
	copy.name = duplicateName(*project, source->name);
	copy.revision = 1;
	std::map<std::string, std::string> participantIds;
	for (Participant& participant : copy.participants) {
		const std::string oldId = participant.id;
		participant.id = matters::generate_consilium_participant_id();
		participantIds.emplace(oldId, participant.id);
	}
	for (PlanStage& stage : copy.stages) {
		stage.id = matters::generate_consilium_stage_id();
		for (std::string& participantId : stage.participant_ids) participantId = participantIds.at(participantId);
	}
	stamp(copy, true);
	project->consilium_configurations.push_back(copy);
	project->updated_at = copy.updated_at;
	matters::save_matter(workspace, *project);
	return viewFor(*project, copy);
}

ConfigurationView import_file(const std::string& projectId, const std::filesystem::path& path) {
	std::ifstream input(path, std::ios::binary);
	if (input.is_open() == false) throw std::runtime_error("Singularity could not open this Consilium file.");
	input.seekg(0, std::ios::end);
	const std::streamoff size = input.tellg();
	if (size < 0 || size > 1024 * 1024) throw std::runtime_error("This Consilium file is too large.");
	input.seekg(0, std::ios::beg);
	Json root;
	try {
		input >> root;
	} catch (const std::exception&) {
		throw std::runtime_error("This Consilium file is not valid JSON.");
	}

	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const std::filesystem::path workspace = node::require_active_workspace_path();
	auto project = matters::load_matter(workspace, projectId);
	if (!project) throw std::runtime_error("This Project no longer exists.");
	Configuration configuration = configurationFromPortable(root, projectId);
	if (std::any_of(project->consilium_configurations.begin(), project->consilium_configurations.end(),
			[&](const Configuration& value) { return value.name == configuration.name; })) {
		configuration.name = duplicateName(*project, configuration.name);
	}
	validateConfiguration(*project, configuration);
	stamp(configuration, true);
	project->consilium_configurations.push_back(configuration);
	project->updated_at = configuration.updated_at;
	matters::save_matter(workspace, *project);
	return viewFor(*project, configuration);
}

void export_file(const std::string& projectId, const std::string& configurationId,
		const std::filesystem::path& path) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const std::vector<matters::Matter> projects = matters::load_matters(node::require_active_workspace_path());
	const auto project = std::find_if(projects.begin(), projects.end(),
		[&](const matters::Matter& value) { return value.id == projectId; });
	if (project == projects.end()) throw std::runtime_error("This Project no longer exists.");
	const auto configuration = std::find_if(project->consilium_configurations.begin(),
		project->consilium_configurations.end(), [&](const Configuration& value) {
			return value.id == configurationId;
		});
	if (configuration == project->consilium_configurations.end()) {
		throw std::runtime_error("This Consilium configuration no longer exists.");
	}
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (output.is_open() == false) throw std::runtime_error("Singularity could not create the Consilium export.");
	output << portableJson(*configuration).dump(2) << '\n';
	output.close();
	if (output.fail()) throw std::runtime_error("Singularity could not finish the Consilium export.");
}

} // namespace singularity::consilium
