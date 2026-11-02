#pragma once

#include "Context/SourceMarkers/SourceMarkers.hpp"
#include "ExternalResearch/Types/ExternalResearchTypes.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Matter/Types/Matter.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace singularity::context {

// The label a person reads for a cited Project source: the document's current
// name (its Project-relative path when two available documents share a name),
// page or heading when known, and "(no longer available)" for a source that is
// gone. Internal handles ("DOC_2", "CHUNK_7") and ids never appear in it.
class CurrentSourceLabels {
public:
	explicit CurrentSourceLabels(const matters::Matter &project) : project_(project) {
		for (const matters::MatterDocument &document : project_.documents)
			if (matters::document_lifecycle_state(document) != matters::DocumentLifecycleState::Unavailable)
				document_ids_by_name_[document.name].insert(document.id);
	}

	[[nodiscard]] std::string label(const ResolvedSourceReference &reference) const {
		ResolvedSourceReference display = reference;
		const auto document = std::find_if(project_.documents.begin(), project_.documents.end(),
			[&](const matters::MatterDocument &candidate) { return candidate.id == reference.document_id; });
		const bool available = document != project_.documents.end()
			&& matters::document_lifecycle_state(*document) != matters::DocumentLifecycleState::Unavailable;
		if (available) {
			const auto sameName = document_ids_by_name_.find(document->name);
			display.document_name = sameName != document_ids_by_name_.end() && sameName->second.size() > 1
					&& !document->relative_path.empty()
				? document->relative_path : document->name;
		}
		std::string text = format_reference_label(display);
		if (!available) text += " (no longer available)";
		return text;
	}

private:
	const matters::Matter &project_;
	std::map<std::string, std::set<std::string>> document_ids_by_name_;
};

namespace source_labels_detail {

inline std::string with_external_labels(const std::string &rendered,
		const std::vector<research::ExternalEvidenceReference> &external) {
	std::string result;
	std::size_t position = 0;
	while (position < rendered.size()) {
		const std::size_t start = rendered.find("[EXTERNAL:", position);
		if (start == std::string::npos) {
			result.append(rendered, position, std::string::npos);
			break;
		}
		result.append(rendered, position, start - position);
		const std::size_t end = rendered.find(']', start);
		if (end == std::string::npos) {
			result.append(rendered, start, std::string::npos);
			break;
		}
		const std::string marker = rendered.substr(start, end - start + 1);
		const auto evidence = std::find_if(external.begin(), external.end(),
			[&](const research::ExternalEvidenceReference &entry) { return entry.marker() == marker; });
		if (evidence == external.end()) {
			result += kUnverifiedSourceLabel;
		} else {
			const std::string title = evidence->title.value_or(evidence->domain.value_or(std::string()));
			result += "(" + (title.empty() ? evidence->url : title + ", " + evidence->url) + ")";
		}
		position = end + 1;
	}
	return result;
}

} // namespace source_labels_detail

// A result's text as a person reads it outside its operation (a retained Work
// Product, an export): every Project source handle becomes its current label,
// every cited research source "(title, address)", and a handle the operation
// never supplied "[unverified source reference]". The internal marker grammar
// never survives.
inline std::string source_text_for_people(const std::string &text, const std::vector<ResolvedSourceReference> &cited,
		const std::vector<research::ExternalEvidenceReference> &external, const matters::Matter &project) {
	const CurrentSourceLabels labels(project);
	const std::string rendered = render_source_handles(text, cited,
		[&](const ResolvedSourceReference &reference) { return labels.label(reference); }).text;
	return source_labels_detail::with_external_labels(rendered, external);
}

// The same text as an earlier answer is carried in a chat's history to the
// next request: each run of citations named once, apart from the words around
// it (render_source_handles_apart), so no name in a label is glued to a word.
inline std::string source_text_for_history(const std::string &text, const std::vector<ResolvedSourceReference> &cited,
		const std::vector<research::ExternalEvidenceReference> &external, const matters::Matter &project) {
	const CurrentSourceLabels labels(project);
	const std::string rendered = render_source_handles_apart(text, cited,
		[&](const ResolvedSourceReference &reference) { return labels.label(reference); }).text;
	return source_labels_detail::with_external_labels(rendered, external);
}

} // namespace singularity::context
