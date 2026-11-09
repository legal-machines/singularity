#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "DocumentIntelligence/ReviewResult/ReviewResult.hpp"

namespace singularity::document_intelligence {

struct RevisionOutcome {
	bool isWritten = false;
	std::string problem;
	std::filesystem::path path;
	std::string sha256;
	std::size_t written = 0;
	std::size_t refused = 0;
	// Comments for the margin written and read back on their clauses.
	std::size_t commented = 0;
	// Track Changes is left on in the draft, so later edits are tracked too.
	bool isTrackingOn = false;
};

// A revised draft: a copy of `source` with every writable change written into
// it as a Word tracked change by `author` - the old words struck, the new ones
// inserted beside them, accepted or rejected with Word's own buttons - through
// the same writer every other change to a Word document goes through
// (Document/DocxExtractor apply_docx_document_patch). Each change is written
// on its own, so one that cannot be written is refused and said so, and never
// takes the others with it; each change's writeState and writeNote say what
// happened to it.
//
// The source is never opened for writing. `target` must not exist: a revised
// draft is a new file, never written over another. Nothing is left at
// `target` unless at least one change was written and the file read back as a
// Word document holding them.
RevisionOutcome writeRevisedDocument(const std::filesystem::path& source, const std::filesystem::path& target,
	std::vector<ProposedChange>& changes, const std::string& author);

// The same, with comments for the margin: each a Word comment by `author` on
// the paragraph of its clause, found by the clause's words where they stand
// once, and read back on that clause by the Matter's own reader - or
// refused, and said why. Comments are placed before the changes are written,
// on the source's own words. A draft is kept when a change or a comment is
// in it.
RevisionOutcome writeRevisedDocument(const std::filesystem::path& source, const std::filesystem::path& target,
	std::vector<ProposedChange>& changes, std::vector<MarginComment>& comments, const std::string& author);

} // namespace singularity::document_intelligence
