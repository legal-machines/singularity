#include "Context/SourcePicker/SourcePicker.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace singularity::context::source_picker {
namespace {

using Json = nlohmann::json;

std::string lowered(std::string text) {
	for (char &symbol : text) symbol = static_cast<char>(std::tolower(static_cast<unsigned char>(symbol)));
	return text;
}

std::string trimmed(const std::string &value) {
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return std::string();
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

// The first sentence or line of a digest, whole words, within the bound - and
// nothing of a digest's own headings or bullets.
std::string lead_of(const std::string &digest) {
	std::string text;
	for (char symbol : digest) {
		if (symbol == '\n' || symbol == '\r') { if (!trimmed(text).empty()) break; text.clear(); continue; }
		text += symbol;
	}
	text = trimmed(text);
	while (!text.empty() && (text.front() == '-' || text.front() == '*' || text.front() == '#' || text.front() == ' ')) text.erase(text.begin());
	if (text.size() <= kLeadCharacters) return text;
	std::size_t cut = text.rfind(' ', kLeadCharacters);
	if (cut == std::string::npos || cut < kLeadCharacters / 2) cut = kLeadCharacters;
	while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xc0U) == 0x80U) --cut;
	return trimmed(text.substr(0, cut)) + "…";
}

std::string folder_of(const std::string &relativePath) {
	const auto slash = relativePath.find_last_of('/');
	return slash == std::string::npos ? std::string() : relativePath.substr(0, slash);
}

std::string stem_of(const std::string &name) {
	const auto dot = name.find_last_of('.');
	return dot != std::string::npos && dot > 0 ? name.substr(0, dot) : name;
}

} // namespace

std::vector<ListedDocument> list_documents(const matters::Matter &matter) {
	const bool hasProjectFolder = matter.directory_path.has_value() && !matter.directory_path->empty();
	std::vector<ListedDocument> listed;
	for (const matters::MatterDocument &document : matter.documents) {
		if (!document.available || document.extraction_status != matters::DocumentExtractionState::Ready) continue;
		if (hasProjectFolder && document.source_mode != matters::DocumentSourceMode::Referenced) continue;
		ListedDocument entry;
		entry.handle = "D" + std::to_string(listed.size() + 1);
		entry.document_id = document.id;
		entry.name = document.name;
		entry.folder = folder_of(document.relative_path);
		const auto dot = document.name.find_last_of('.');
		entry.format = dot != std::string::npos ? lowered(document.name.substr(dot + 1)) : std::string();
		const auto digest = std::find_if(matter.memory_document_digests.begin(), matter.memory_document_digests.end(),
			[&](const matters::MemoryDocumentDigest &candidate) { return candidate.document_id == document.id; });
		if (digest != matter.memory_document_digests.end()) entry.lead = lead_of(digest->digest);
		listed.push_back(std::move(entry));
	}
	return listed;
}

bool fits_whole(const matters::Matter &matter, const std::vector<ListedDocument> &listed,
		std::optional<std::size_t> window_tokens) {
	if (!window_tokens || listed.empty()) return false;
	long long characters = 0;
	for (const ListedDocument &entry : listed)
		for (const matters::MatterDocument &document : matter.documents)
			if (document.id == entry.document_id) characters += document.extracted_character_count;
	const long long comfortable = static_cast<long long>(*window_tokens) * 4 * 2 / 5;
	return characters <= std::min(kWholeRoomMaximumCharacters, comfortable);
}

bool worth_picking(const matters::Matter &matter, const std::vector<ListedDocument> &listed) {
	if (listed.size() < kMinimumDocumentsToPick) return false;
	long long characters = 0;
	for (const ListedDocument &entry : listed)
		for (const matters::MatterDocument &document : matter.documents)
			if (document.id == entry.document_id) characters += document.extracted_character_count;
	return characters >= kMinimumCharactersToPick;
}

std::vector<std::string> documents_named_in(const std::string &request, const std::vector<ListedDocument> &documents) {
	const std::string haystack = lowered(request);
	std::vector<std::string> named;
	for (const ListedDocument &document : documents) {
		const std::string stem = stem_of(document.name);
		// A short name would be found in any sentence with the word in it.
		if (stem.size() < 6) continue;
		if (haystack.find(lowered(stem)) == std::string::npos) continue;
		named.push_back(document.document_id);
	}
	return named;
}

std::string pick_prompt(const std::string &request, const std::vector<ListedDocument> &documents,
		const std::string &memory_brief) {
	std::string prompt;
	prompt += "You choose which documents of a legal Project a request needs, before any of them is read. "
		"Below are the request and the list of the Project's documents - names only, with a line on what "
		"some of them hold. Pick the documents the request must be answered or done from: the ones it names, "
		"and the ones its subject rests on - for a request about what changed, an earlier position or a "
		"comparison, the earlier versions as well as the current one. A request about the Project as a whole - "
		"what it is about, where it stands, an overview or summary - rests on every document that carries the "
		"matter (the agreements and their drafts, the decisions and notes on them), not on a note about it. "
		"Whether something was approved, agreed or decided rests on the minutes, resolutions or letters that "
		"would record it. "
		"Pick nothing beside the point, and at most "
		+ std::to_string(kMaximumPicked) + ". If the request needs no document at all (a greeting, a question "
		"about Singularity, a matter of general law), say so with needsNone. Answer only in the required JSON, "
		"with the handles from the list.\n\n";
	prompt += "REQUEST:\n" + trimmed(request) + "\n\n";
	if (!trimmed(memory_brief).empty()) prompt += "WHAT IS REMEMBERED ABOUT THIS PROJECT:\n" + trimmed(memory_brief) + "\n\n";
	prompt += "DOCUMENTS:\n";
	for (const ListedDocument &document : documents) {
		prompt += document.handle + ": " + document.name;
		if (!document.folder.empty()) prompt += "  (in " + document.folder + ")";
		if (!document.lead.empty()) prompt += "\n    " + document.lead;
		prompt += "\n";
	}
	return prompt;
}

providers::GenerationOutputContract pick_output_contract() {
	providers::GenerationOutputContract contract;
	contract.schema_name = "source_pick";
	contract.json_schema = Json{{"type", "object"}, {"additionalProperties", false},
		{"properties", {
			{"documents", {{"type", "array"}, {"items", {{"type", "string"}}}}},
			{"needsNone", {{"type", "boolean"}}},
		}},
		{"required", Json::array({"documents", "needsNone"})}};
	return contract;
}

std::vector<std::string> picked_document_ids(const std::string &reply, const std::vector<ListedDocument> &documents) {
	std::vector<std::string> picked;
	// The JSON may come wrapped in reasoning or a code fence: the first object
	// in the text is the answer.
	const auto open = reply.find('{');
	const auto close = reply.rfind('}');
	if (open == std::string::npos || close == std::string::npos || close <= open) return picked;
	const Json value = Json::parse(reply.substr(open, close - open + 1), nullptr, false);
	if (!value.is_object()) return picked;
	if (value.value("needsNone", false)) return picked;
	if (!value.contains("documents") || !value.at("documents").is_array()) return picked;
	std::unordered_set<std::string> seen;
	for (const Json &item : value.at("documents")) {
		if (!item.is_string()) continue;
		const std::string handle = trimmed(item.get<std::string>());
		for (const ListedDocument &document : documents) {
			if (lowered(document.handle) != lowered(handle) && lowered(document.name) != lowered(handle)) continue;
			if (seen.insert(document.document_id).second) picked.push_back(document.document_id);
			break;
		}
		if (picked.size() >= kMaximumPicked) break;
	}
	return picked;
}

} // namespace singularity::context::source_picker
