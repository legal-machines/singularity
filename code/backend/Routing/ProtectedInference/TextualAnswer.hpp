#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

// A model without native tools answers through the textual protocol: its
// structured answer travels as ONE string, {"type":"final","text":"{...}"}.
// Restored as one piece, the operands of its localFacts would come back as the
// values they stand for, and the operations executor could relate none of
// them. Read as the structured answer it is, every field is restored as the
// structured path restores it and localFacts stay in the symbol plane
// (ProtectedInference's restore_json_strings; the scripted executor mirrors it).
namespace singularity::routing::textual_answer {

// The structured answer a string field carries when it is one with questions
// for Singularity (a non-null "localFacts"); nullopt for any other text, which is
// restored as text exactly as before.
inline std::optional<nlohmann::json> embedded_answer(const std::string &text) {
	const std::size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos || text[first] != '{' || text.find("\"localFacts\"") == std::string::npos)
		return std::nullopt;
	nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
	if (!parsed.is_object() || !parsed.contains("localFacts") || parsed.at("localFacts").is_null()) return std::nullopt;
	return parsed;
}

// A reply that is two or more whole JSON objects, one after another with
// nothing but space between ("{...}\n\n{...}"), is read as its last one:
// the model wrote its answer again (live Luna, 26 Sep 2026 - read whole, it
// was no JSON at all, so its operations went unserved and the lawyer was
// shown the raw text). Nothing else is changed.
inline std::optional<std::string> last_of_whole_objects(const std::string &text) {
	std::vector<std::pair<std::size_t, std::size_t>> objects;
	std::size_t at = 0;
	const auto skip_space = [&] {
		while (at < text.size() && (text[at] == ' ' || text[at] == '\n' || text[at] == '\r' || text[at] == '\t')) ++at;
	};
	skip_space();
	while (at < text.size()) {
		if (text[at] != '{') return std::nullopt;
		const std::size_t start = at;
		int depth = 0;
		bool inString = false;
		for (; at < text.size(); ++at) {
			const char character = text[at];
			if (inString) {
				if (character == '\\') ++at;
				else if (character == '"') inString = false;
				continue;
			}
			if (character == '"') inString = true;
			else if (character == '{' || character == '[') ++depth;
			else if (character == '}' || character == ']') {
				if (--depth == 0 && character == '}') {
					++at;
					break;
				}
			}
		}
		if (depth != 0) return std::nullopt;
		objects.emplace_back(start, at - start);
		skip_space();
	}
	if (objects.size() < 2) return std::nullopt;
	for (const auto &[start, length] : objects)
		if (!nlohmann::json::parse(text.substr(start, length), nullptr, false).is_object()) return std::nullopt;
	return text.substr(objects.back().first, objects.back().second);
}

} // namespace singularity::routing::textual_answer
