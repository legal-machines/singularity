#include "PapinianGuard/ProtectedPreview/ProtectedPreview.hpp"

#include <algorithm>
#include <unordered_map>

namespace singularity::protected_api::papinian {
namespace {

constexpr std::size_t kExcerptWindow = 60;

std::string category_for(const RequestMappingEntry &entry) {
	if (entry.type) return to_string(*entry.type);
	return entry.source == DetectionSource::CustomTerm ? "CUSTOM_TERM" : "LEGACY_URL";
}

std::string display_label(const std::string &category) {
	std::string label = category;
	std::replace(label.begin(), label.end(), '_', ' ');
	return label;
}

// A running per-category first-appearance index, e.g. the second PERSON
// mapping entry gets "PERSON_2" - stable across repeated calls against the
// identical ProtectionResult, unlike the technical placeholder (which
// embeds a fresh request_namespace every call).
std::vector<std::string> occurrence_ids_for(const std::vector<RequestMappingEntry> &mapping) {
	std::unordered_map<std::string, int> counters;
	std::vector<std::string> ids;
	ids.reserve(mapping.size());
	for (const auto &entry : mapping) {
		const auto category = category_for(entry);
		ids.push_back(category + "_" + std::to_string(++counters[category]));
	}
	return ids;
}

// Locates `value` inside whichever real source material this exact request
// actually drew from. Never guesses: an empty/absent match is reported as
// "context" rather than attributed to a source that does not contain it.
struct LocatedOrigin {
	std::string kind;
	std::optional<std::string> object_id;
	const std::string *text = nullptr;
	std::size_t position = std::string::npos;
};

LocatedOrigin locate_origin(const std::string &value, const OriginSources &sources) {
	if (!value.empty()) {
		const auto request_position = sources.raw_request_text.find(value);
		if (request_position != std::string::npos) {
			return LocatedOrigin{"request", std::nullopt, &sources.raw_request_text, request_position};
		}
		for (const auto &document : sources.documents) {
			const auto document_position = document.content.find(value);
			if (document_position != std::string::npos) {
				return LocatedOrigin{"document", document.display_name, &document.content, document_position};
			}
		}
	}
	return LocatedOrigin{"context", std::nullopt, nullptr, std::string::npos};
}

} // namespace

ProtectedPreviewSummary summarize_for_preview(const ProtectionResult &result, const OriginSources &sources) {
	ProtectedPreviewSummary summary;
	summary.human_readable_text = result.protected_text;
	if (result.mapping.empty()) return summary;

	const auto occurrence_ids = occurrence_ids_for(result.mapping);

	// Longest placeholder first, matching PapinianGuard::restore()'s own
	// rationale: a shorter placeholder can be a prefix of a longer one.
	std::vector<std::size_t> order(result.mapping.size());
	for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
	std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
		return result.mapping[left].placeholder.size() > result.mapping[right].placeholder.size();
	});

	for (const auto index : order) {
		const auto &entry = result.mapping[index];
		const auto category = category_for(entry);
		const auto display_placeholder =
				display_label(category) + " " + occurrence_ids[index].substr(category.size() + 1);
		// An organisation's short name goes as its short part, "[[E3.short]]":
		// the same value, shown with the same label.
		const std::string shortPart = entry.placeholder.size() > 2
			? entry.placeholder.substr(0, entry.placeholder.size() - 2) + ".short]]" : std::string();
		for (const std::string &written : {entry.placeholder, shortPart}) {
			if (written.empty()) continue;
			std::size_t position = 0;
			while ((position = summary.human_readable_text.find(written, position)) != std::string::npos) {
				summary.human_readable_text.replace(position, written.size(), display_placeholder);
				position += display_placeholder.size();
			}
		}
	}

	summary.occurrences.reserve(result.mapping.size());
	for (std::size_t index = 0; index < result.mapping.size(); ++index) {
		const auto &entry = result.mapping[index];
		const auto category = category_for(entry);
		const auto origin = locate_origin(entry.original, sources);
		summary.occurrences.push_back(ProtectedOccurrenceSummary{
				occurrence_ids[index],
				category,
				display_label(category) + " " + occurrence_ids[index].substr(category.size() + 1),
				origin.kind,
				origin.object_id,
		});
	}
	return summary;
}

std::optional<RevealedOccurrence> reveal_occurrence(
		const ProtectionResult &result, const OriginSources &sources, const std::string &occurrence_id) {
	const auto occurrence_ids = occurrence_ids_for(result.mapping);
	const auto found = std::find(occurrence_ids.begin(), occurrence_ids.end(), occurrence_id);
	if (found == occurrence_ids.end()) return std::nullopt;

	const auto index = static_cast<std::size_t>(found - occurrence_ids.begin());
	const auto &entry = result.mapping[index];
	const auto category = category_for(entry);
	const auto origin = locate_origin(entry.original, sources);

	RevealedOccurrence revealed;
	revealed.occurrence_id = occurrence_id;
	revealed.category = category;
	revealed.display_placeholder = display_label(category) + " " + occurrence_id.substr(category.size() + 1);
	revealed.origin_kind = origin.kind;
	revealed.origin_object_id = origin.object_id;

	if (origin.text == nullptr) {
		revealed.excerpt_match = entry.original;
		return revealed;
	}

	const auto &text = *origin.text;
	const auto match_start = origin.position;
	const auto match_end = match_start + entry.original.size();
	const auto before_start = match_start > kExcerptWindow ? match_start - kExcerptWindow : 0;
	const auto after_end = std::min(text.size(), match_end + kExcerptWindow);

	revealed.excerpt_before = text.substr(before_start, match_start - before_start);
	revealed.excerpt_match = text.substr(match_start, entry.original.size());
	revealed.excerpt_after = text.substr(match_end, after_end - match_end);
	return revealed;
}

} // namespace singularity::protected_api::papinian
