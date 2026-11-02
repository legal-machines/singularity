#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

// A member's place in the firm (ORG-1): the office it holds and the practice it
// works in, for a person and an AI colleague alike, and the firm's one scale of
// positions that every office ranks on (D-059, PHIL-2). An AI colleague holds
// an office of its own ("Practice Group Leader") and ranks with a position of
// the scale ("Partner"), as a person holds a position. Facts of the whole
// firm, never of one Matter (the owner, 2026-10-26: "she exists not within one
// project but within the whole firm").
//
// Header-only: every domain that says where a member stands reads it, and none
// takes a dependency for it.
namespace singularity::organisation {

struct MemberOffice {
	// A person's position ("Senior Associate"), or an AI colleague's office
	// ("Practice Group Leader").
	std::string position;
	// The practice it works in ("Labor & Employment").
	std::string practice;
};

// The firm's positions, from the top (D-059).
inline const std::vector<std::string>& firm_positions() {
	static const std::vector<std::string> positions{"Partner", "Senior Counsel", "Of Counsel", "Counsel", "Senior Associate",
		"Associate", "Paralegal"};
	return positions;
}

inline std::string lowered_words(const std::string& text) {
	std::string lowered = text;
	std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	return lowered;
}

// Where an office stands on the scale: 0 for Partner, down to the last; -1
// for an office the scale does not name. A title that holds a position's name
// as a whole word stands with that position, the longest name first ("Senior
// Associate (Labor)" is a Senior Associate, never an Associate).
inline int position_rank(const std::string& office) {
	const std::string words = " " + lowered_words(office) + " ";
	int best = -1;
	std::size_t bestLength = 0;
	const std::vector<std::string>& positions = firm_positions();
	for (std::size_t index = 0; index < positions.size(); ++index) {
		const std::string name = lowered_words(positions[index]);
		std::size_t at = words.find(name);
		while (at != std::string::npos) {
			const bool isWhole = std::isalnum(static_cast<unsigned char>(words[at - 1])) == 0
				&& std::isalnum(static_cast<unsigned char>(words[at + name.size()])) == 0;
			if (isWhole == true && name.size() > bestLength) {
				best = static_cast<int>(index);
				bestLength = name.size();
			}
			at = words.find(name, at + 1);
		}
	}
	return best;
}

// The position's members, as a sentence names them ("the partners").
inline std::string position_members(const std::string& position) {
	const int rank = position_rank(position);
	if (rank < 0) return std::string();
	static const char* members[] = {"the partners", "the senior counsel", "of counsel", "counsel", "the senior associates",
		"the associates", "the paralegals"};
	return members[rank];
}

// An office as a line says it: "Senior Associate, Labor & Employment".
inline std::string office_words(const MemberOffice& office) {
	if (office.position.empty() == true) return office.practice;
	if (office.practice.empty() == true) return office.position;
	return office.position + ", " + office.practice;
}

// An AI colleague's office as the team list says it, its practice and area as
// a person's office carries its practice: "AI colleague, Senior Associate,
// Technology & IP: IP and open source". A lead told only "Senior Associate"
// five times gave the IP review to the commercial associate (2026-11-02).
inline std::string colleague_office_words(const std::string& title, const std::string& practice, const std::string& area) {
	const std::string office = office_words(MemberOffice{title, practice});
	std::string words = office.empty() == true ? std::string("AI colleague") : "AI colleague, " + office;
	if (area.empty() == false) words += ": " + area;
	return words;
}

// Where a member stands to a colleague who ranks with `ranks_with`, as the
// colleague is told it - rank carries the work, as the owner gives it
// (2026-10-26: "since she heads a practice, she gives tasks to the people
// below her"): "who ranks below you and takes work from you", "who ranks above
// you and may give you work", "who ranks with you"; nothing where either is
// off the scale.
inline std::string standing_words(const std::string& ranks_with, const std::string& member_position) {
	const int mine = position_rank(ranks_with);
	const int theirs = position_rank(member_position);
	if (mine < 0 || theirs < 0) return std::string();
	if (theirs > mine) return "who ranks below you and takes work from you";
	if (theirs < mine) return "who ranks above you and may give you work";
	return "who ranks with you";
}

} // namespace singularity::organisation
