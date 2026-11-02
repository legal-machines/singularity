#pragma once

#include "Routing/Types/ModelTarget.hpp"

#include <optional>
#include <string>
#include <vector>

namespace singularity::consilium {

enum class StageMode { Sequential, Parallel };
enum class SourceMode { RelevantProject, SelectedDocuments };

struct Participant {
	std::string id;
	std::string role;
	std::string ai_setup_role;
	std::optional<routing::ModelTarget> explicit_model;
};

struct PlanStage {
	std::string id;
	std::string name;
	StageMode mode = StageMode::Sequential;
	std::vector<std::string> participant_ids;
};

struct SourcePolicy {
	SourceMode mode = SourceMode::RelevantProject;
	std::vector<std::string> selected_document_ids;
	bool requires_selection = false;
};

struct Configuration {
	std::string id;
	std::string project_id;
	std::string name;
	std::string objective;
	std::vector<Participant> participants;
	std::vector<PlanStage> stages;
	SourcePolicy source_policy;
	long long revision = 1;
	std::string created_at;
	std::string updated_at;
	std::string actor_id;
	std::string operation_id;
};

inline const char* to_string(StageMode mode) {
	return mode == StageMode::Parallel ? "parallel" : "sequential";
}

inline std::optional<StageMode> stage_mode_from_string(const std::string& value) {
	if (value == "sequential") return StageMode::Sequential;
	if (value == "parallel") return StageMode::Parallel;
	return std::nullopt;
}

inline const char* to_string(SourceMode mode) {
	return mode == SourceMode::SelectedDocuments ? "selectedDocuments" : "relevantProject";
}

inline std::optional<SourceMode> source_mode_from_string(const std::string& value) {
	if (value == "relevantProject") return SourceMode::RelevantProject;
	if (value == "selectedDocuments") return SourceMode::SelectedDocuments;
	return std::nullopt;
}

} // namespace singularity::consilium
