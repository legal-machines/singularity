#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "DocumentIntelligence/ReviewResult/ReviewResult.hpp"
#include "DocumentIntelligence/Types/LegalDocument.hpp"

namespace singularity::document_intelligence {

// What a check found, in the lawyer's words.
struct IntegrityFinding {
	// "problem": the draft is not whole or not consistent as written;
	// "attention": for the lawyer to look at; "note": stood in the source
	// already, and the draft did not cause it.
	std::string severity;
	std::string text;
	// The places, as the agreement numbers them: "Section 6.1".
	std::vector<std::string> places;
};

struct IntegrityCheckResult {
	// "completeness", "unchanged", "numbering", "definitions", "references".
	std::string name;
	std::string title;
	// "passed", "attention" or "failed".
	std::string state;
	// One sentence of what was checked and what came of it.
	std::string summary;
	std::vector<IntegrityFinding> findings;
};

// The written draft checked against its source by Singularity itself, apart
// from the model and apart from whether its words were anchored: whether the
// draft is whole and consistent, never whether its changes are good law.
struct IntegrityReport {
	bool isChecked = false;
	// Why the draft could not be checked.
	std::string problem;
	std::vector<IntegrityCheckResult> checks;
};

// The checks, on the source's structure and the draft's as Word would show it
// with every change accepted (LegalStructure reads it so):
//   - completeness: every clause and heading of the source is in the draft;
//   - unchanged: every block no written change touched reads word for word as
//     in the source, and nothing was added that no change added;
//   - numbering: the draft's numbers run without a gap, no numbered clause is
//     empty, and every heading has a clause under it;
//   - definitions: a term the draft uses is defined there, and a term it
//     defines is still used;
//   - references: every Section and Exhibit the draft refers to exists in it,
//     those already missing from the source said apart, and a reference to a
//     clause the draft omits said too;
//   - parties: every party the draft names with its legal form is written
//     exactly as the source writes it - or, where the source is a colleague's
//     own draft, as the Matter's documents write it - never with another legal
//     form, with no mark of the text around it, and in the capacity they give
//     it (D-172) - in body, tables, headers and tracked changes alike. A failed
//     one keeps the draft from being offered for acceptance.
// Where the parties' own writings come from (D-172). The source, unless it is a
// colleague's own draft: one carried "Coastline Logistics Corp." from version
// to version while every pleading of the Matter writes "Coastline Logistics,
// Inc." (2026-10-31). Then the Matter's own documents decide.
struct PartyAuthority {
	bool isSourceAuthoritative = true;
	// The words of each document whose writings of a party are authoritative.
	std::vector<std::string> texts;
};

IntegrityReport checkDraftIntegrity(const LegalDocument& source, const LegalDocument& draft,
	const std::vector<ProposedChange>& changes, const PartyAuthority& authority = PartyAuthority());

// Each writing of a party in `source` - a colleague's own draft - that the
// Matter's documents write otherwise, with the one writing they give it in the
// same case ("Coastline Logistics Corp." to "Coastline Logistics, Inc.",
// "COASTLINE LOGISTICS CORP." to "COASTLINE LOGISTICS, INC."). Empty where the
// source is authoritative itself, and never a guess: a party the documents
// write two ways in one case is left for the check to say.
std::map<std::string, std::string> partyCorrections(const LegalDocument& source, const PartyAuthority& authority);

nlohmann::json integrityReportToJson(const IntegrityReport& report);
IntegrityReport integrityReportFromJson(const nlohmann::json& value);

} // namespace singularity::document_intelligence
