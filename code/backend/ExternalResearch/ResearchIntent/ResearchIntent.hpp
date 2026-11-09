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
		"\320\261\320\265\320\267 \320\270\320\275\321\202\320\265\321\200\320\275\320\265\321\202\320\260", "\320\261\320\265\320\267 \320\270\321\201\320\277\320\276\320\273\321\214\320\267\320\276\320\262\320\260\320\275\320\270\321\217 \320\270\320\275\321\202\320\265\321\200\320\275\320\265\321\202\320\260", "\320\261\320\265\320\267 \320\277\320\276\320\270\321\201\320\272\320\260 \320\262 \320\270\320\275\321\202\320\265\321\200\320\275\320\265\321\202\320\265", "\320\275\320\265 \320\270\321\201\320\277\320\276\320\273\321\214\320\267\321\203\320\271 \320\270\320\275\321\202\320\265\321\200\320\275\320\265\321\202",
		"\320\275\320\265 \320\270\321\201\320\277\320\276\320\273\321\214\320\267\321\203\320\271\321\202\320\265 \320\270\320\275\321\202\320\265\321\200\320\275\320\265\321\202", "\320\275\320\265 \320\270\321\201\320\277\320\276\320\273\321\214\320\267\320\276\320\262\320\260\321\202\321\214 \320\270\320\275\321\202\320\265\321\200\320\275\320\265\321\202", "\320\275\320\265 \320\270\321\211\320\270 \320\262 \320\270\320\275\321\202\320\265\321\200\320\275\320\265\321\202\320\265", "\320\275\320\265 \320\270\321\211\320\270\321\202\320\265 \320\262 \320\270\320\275\321\202\320\265\321\200\320\275\320\265\321\202\320\265",
	});
	if (explicitWeb == true && explicitOff == true) return ResearchMode::Contradictory;
	if (explicitOff == true) return ResearchMode::Off;
	if (explicitWeb == true) return ResearchMode::Required;
	return ResearchMode::Auto;
}

} // namespace singularity::research
