#pragma once

#include "ExternalResearch/Types/ExternalResearchTypes.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace singularity::documents {

// Durable working state embedded in the one canonical MatterTask. The typed
// envelope prevents an in-flight professional operation from degenerating
// into a replay prompt. `draft` is the domain-owned, versioned semantic
// checkpoint produced by ConnectedDocumentAction; it never contains a
// Papinian restoration map or provider response transcript.
struct DocumentOperationCheckpoint {
	int schema_version = 3;
	std::string stage = "drafting";
	nlohmann::json draft = nlohmann::json::object();
	std::vector<research::ExternalEvidenceReference> external_evidence;
	research::OperationExecutionProgress progress;
	// Backend-authored write-ahead record. `prepared` is durable before file
	// mutation; `committed` includes the reconciled canonical version/hash.
	nlohmann::json mutation_commit = nullptr;
};

} // namespace singularity::documents
