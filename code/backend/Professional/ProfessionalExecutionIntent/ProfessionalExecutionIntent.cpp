#include "Professional/ProfessionalExecutionIntent/ProfessionalExecutionIntent.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace singularity::professional {
namespace {

ProfessionalCapability capability_from_string(const std::string &value) {
	if (value == "product_state") return ProfessionalCapability::ProductState;
	if (value == "generative") return ProfessionalCapability::Generative;
	throw std::runtime_error("The professional capability is invalid.");
}

} // namespace

// No text is classified here: every natural-language request is Generative
// and reaches one substantive model call, which decides what the request
// means (MODEL DECIDES WHAT). A former keyword check routed any sentence that
// combined words such as "provider"/"model" with "which"/"selected" to a
// canned product-state answer - including legal questions like "Which
// obligations does the service provider have under the selected agreement?".
// The model now answers questions about Singularity's own execution setting from
// the machine-owned fact the request carries (see prepare_task_request()).
// ProductState stays a valid persisted value for older operations only.
ProfessionalExecutionIntent resolve_professional_execution_intent(
		const std::string &objective, const std::vector<std::string> &selected_document_ids) {
	ProfessionalExecutionIntent intent;
	intent.objective = objective;
	for (const std::string &id : selected_document_ids) intent.sources.push_back({id});
	intent.capability = ProfessionalCapability::Generative;
	return intent;
}

nlohmann::json professional_execution_intent_json(const ProfessionalExecutionIntent &intent) {
	nlohmann::json sources = nlohmann::json::array();
	for (const SourceReference &source : intent.sources) sources.push_back({{"documentId", source.document_id}});
	nlohmann::json result;
	result["objective"] = intent.objective;
	result["sources"] = std::move(sources);
	result["capability"] = to_string(intent.capability);
	return result;
}

ProfessionalExecutionIntent professional_execution_intent_from_json(const nlohmann::json &value) {
	if (!value.is_object()) throw std::runtime_error("The professional execution intent must be an object.");
	ProfessionalExecutionIntent intent;
	intent.objective = value.at("objective").get<std::string>();
	if (value.contains("sources")) for (const auto &source : value.at("sources"))
		intent.sources.push_back({source.at("documentId").get<std::string>()});
	intent.capability = capability_from_string(value.at("capability").get<std::string>());
	return intent;
}

const char *to_string(ProfessionalCapability capability) noexcept {
	switch (capability) {
		case ProfessionalCapability::ProductState: return "product_state";
		case ProfessionalCapability::Generative: return "generative";
	}
	return "generative";
}

} // namespace singularity::professional
