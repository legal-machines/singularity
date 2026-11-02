#pragma once

#include "ExternalResearch/Types/ExternalResearchTypes.hpp"

#include "Platform/TextFold/TextFold.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <vector>

namespace singularity::research {

// The user's instruction as control phrases are matched against it: case-folded
// in every script (utf8proc), typographic apostrophes as ASCII, and runs of
// whitespace as one space.
inline std::string folded_user_instruction(const std::string &instruction) {
	const std::string folded = platform::folded_text(instruction);
	std::string result;
	result.reserve(folded.size());
	bool pendingSpace = false;
	for (std::size_t index = 0; index < folded.size(); ++index) {
		const unsigned char character = static_cast<unsigned char>(folded[index]);
		if (std::isspace(character) != 0) {
			pendingSpace = !result.empty();
			continue;
		}
		if (pendingSpace) result.push_back(' ');
		pendingSpace = false;
		// U+2019 RIGHT SINGLE QUOTATION MARK reads as an apostrophe.
		if (character == 0xE2 && index + 2 < folded.size() && static_cast<unsigned char>(folded[index + 1]) == 0x80
				&& static_cast<unsigned char>(folded[index + 2]) == 0x99) {
			result.push_back('\'');
			index += 2;
			continue;
		}
		result.push_back(static_cast<char>(character));
	}
	return result;
}

// Deterministic user CONTROL only, never semantic routing: an explicit @web
// instruction requires external research, and an explicit instruction not to
// use the internet forbids it. Everything else is Auto - whether external
// evidence is actually needed is the model's own typed judgment (see the
// professional response's externalResearchRequest and ResearchToolLoop.hpp's
// model-native research escalation), never inferred here from words such as
// "current" or "latest".
inline ResearchMode resolve_research_mode(const std::string &userInstruction) {
	const std::string value = folded_user_instruction(userInstruction);
	auto containsAny = [&](const std::vector<std::string> &needles) {
		for (const std::string &needle : needles)
			if (value.find(needle) != std::string::npos) return true;
		return false;
	};
	const bool explicitWeb = std::regex_search(value,
		std::regex(R"((^|[\s[:punct:]])@web($|[\s[:punct:]]))", std::regex::ECMAScript));
	const bool explicitOff = containsAny({
		"no internet", "without internet", "without the internet", "do not use the internet",
		"don't use the internet", "do not use the web", "don't use the web", "no web search",
		"without web search", "offline only",
		"без интернета", "без использования интернета", "без поиска в интернете", "не используй интернет",
		"не используйте интернет", "не использовать интернет", "не ищи в интернете", "не ищите в интернете",
	});
	if (explicitWeb == true && explicitOff == true) return ResearchMode::Contradictory;
	if (explicitOff == true) return ResearchMode::Off;
	if (explicitWeb == true) return ResearchMode::Required;
	return ResearchMode::Auto;
}

} // namespace singularity::research
