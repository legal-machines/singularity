#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <vector>

namespace singularity::professional {

// STRICT PRODUCT LAW (real product regression, this session): WHAT
// professional work a request calls for is never decided by Singularity matching
// the instruction's text against a hand-written multilingual phrase/keyword
// dictionary ("contains_any"-style hardcoding). A real live acceptance run
// this same session proved such a dictionary has BOTH failure directions at
// once and cannot be patched into safety by adding more phrases: it silently
// invented a document mutation for "Дай мне краткое содержание выбранного
// документа" (nothing in that sentence should ever touch a file) while
// simultaneously failing to recognize "Make the recommendations shorter and
// stricter, and raise the severity" as the same-object follow-up edit it
// obviously is (no "update"/"edit" root, no "риск" root - a real, working
// paraphrase a lawyer would actually type). Growing the dictionary to cover
// one case reliably breaks the other, because natural language has no fixed,
// enumerable vocabulary for "this is a mutation" - only a model that actually
// reads the sentence does.
//
// This is now the sole authority split: Singularity resolves ONLY structural,
// non-semantic facts before generation - what the user explicitly selected
// as read scope (sources, below). No request text is classified: the former
// ProfessionalCapability::ProductState keyword check (a canned answer for
// "which model is this Chat using") also swallowed legal questions and is
// gone; ProductState remains only as a persisted value of older operations.
// Every request -
// summarize, explain, compare, research, create an artifact, update one,
// fill or update a structured professional record - reaches exactly ONE
// substantive model call (see Document/ProfessionalArtifactAction/
// ProfessionalArtifactAction.hpp's own unified professional-response
// contract), and THE MODEL's own returned actionType is what Singularity then
// validates and executes. Singularity remains authoritative over sources,
// identities, available capabilities, target/destination resolution,
// permissions, filesystem paths, versions/fingerprints, approval,
// persistence and commit - never over which kind of professional work this
// instruction calls for.
enum class ProfessionalCapability { ProductState, Generative };

struct SourceReference {
	std::string document_id;
};

// The durable, provider-independent statement of what Singularity resolved
// BEFORE generation. Deliberately thin: no operations/destination/ambiguity
// fields here any more (those used to be guessed from text) - a Generative
// request's actual shape is resolved from the model's own single response,
// not predicted ahead of it. `sources` is a structural fact (the user's own
// explicit selection), never inferred from instruction wording.
struct ProfessionalExecutionIntent {
	std::string objective;
	std::vector<SourceReference> sources;
	ProfessionalCapability capability = ProfessionalCapability::Generative;
};

[[nodiscard]] ProfessionalExecutionIntent resolve_professional_execution_intent(
	const std::string &objective, const std::vector<std::string> &selected_document_ids);

[[nodiscard]] nlohmann::json professional_execution_intent_json(
	const ProfessionalExecutionIntent &intent);
[[nodiscard]] ProfessionalExecutionIntent professional_execution_intent_from_json(
	const nlohmann::json &value);

[[nodiscard]] const char *to_string(ProfessionalCapability capability) noexcept;

} // namespace singularity::professional
