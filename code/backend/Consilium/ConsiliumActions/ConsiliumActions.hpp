#pragma once

#include "Consilium/Types/ConsiliumConfiguration.hpp"
#include "Routing/AiSetup/AiSetup.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace singularity::consilium {

struct EffectiveParticipantAssignment {
	std::string participant_id;
	std::optional<routing::ModelTarget> model;
	std::string source;
};

struct ConfigurationView {
	Configuration configuration;
	std::vector<EffectiveParticipantAssignment> effective_assignments;
};

struct CreateConfigurationCommand {
	std::string project_id;
	std::string name;
	std::string objective;
};

struct UpdateConfigurationCommand {
	std::string project_id;
	std::string configuration_id;
	long long expected_revision = 0;
	std::string name;
	std::string objective;
	std::vector<Participant> participants;
	std::vector<PlanStage> stages;
	SourcePolicy source_policy;
};

std::vector<ConfigurationView> list(const std::string& projectId);
ConfigurationView create(CreateConfigurationCommand command);
ConfigurationView update(UpdateConfigurationCommand command);
ConfigurationView duplicate(const std::string& projectId, const std::string& configurationId);
ConfigurationView import_file(const std::string& projectId, const std::filesystem::path& path);
void export_file(const std::string& projectId, const std::string& configurationId,
		const std::filesystem::path& path);

} // namespace singularity::consilium
