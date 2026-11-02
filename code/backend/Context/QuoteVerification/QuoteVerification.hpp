#pragma once

#include "Context/SourceMarkers/SourceMarkers.hpp"
#include "Platform/PhraseMatch/PhraseMatch.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace singularity::context {

// A passage an answer put in quotation marks, and what became of checking it
// against the document it was attributed to.
struct QuotedPassage {
	// The quoted text as the answer wrote it, without its quotation marks.
	std::string quote;
	// The citation this quote was checked against: empty when the answer quoted
	// something without citing anything near it.
	std::string document_id;
	std::string chunk_id;
	std::string document_name;
	// True only when the quoted words stand in that passage, in that order,
	// differing at most in whitespace. Then `start`/`length` are the exact
	// bytes of the passage they stand at, so the document opens at the quote.
	bool verified = false;
	std::size_t start = 0;
	std::size_t length = 0;
	// The passage's own words at that place, byte for byte. Where the answer
	// wrote the same words with different quotation marks, dashes, spacing or
	// case - which is what a model does when it nests a quotation inside its
	// own - Singularity puts the document's wording back, and `repaired` says so.
	// A quotation whose words differ is never repaired: it is unverified.
	std::string exact;
	bool repaired = false;
	// True when the words were not in the passage cited beside them but stand
	// in another passage this same operation supplied: the quotation is bound
	// to that passage instead - where the words really are - and is verified.
	bool rebound = false;
	// True when the quotation's own paragraph cites a source: the answer says
	// where the words come from. Quoted words in a paragraph that cites nothing
	// - a clause the model proposes, a phrase it coins - are checked all the
	// same, but they are not presented as a quotation of evidence, and an
	// unverified one is not marked as a source problem.
	bool attributed = false;
};

// Every quotation in `answer`, checked against the passages the answer itself
// cited. Nothing here is a judgement about the answer's reasoning: a quotation
// either stands in the cited passage or it does not, and a lawyer is told
// which, rather than being left to check by hand.
//
// What counts as a quotation: text inside "..." or a typographic pair, of at
// least a few words - a phrase, not a word or a defined term in inverted
// commas. A quotation is checked against the citations standing in the same
// paragraph; where the paragraph cites nothing, it is checked against every
// passage this answer cited, and attributed to the one it is found in.
//
// Typography never decides: a model reflows what it quotes and re-marks the
// quotation marks inside it, so runs of whitespace count as one space, every
// kind of quotation mark and apostrophe counts as the same character, every
// kind of dash as the same dash, and case is ignored. The WORDS must be the
// passage's own. An ellipsis inside a quotation is read as a gap - the parts
// around it must stand in the passage, in order.
//
// `passage_text` gives the full text of a cited passage, or nothing when it
// cannot be read right now (the file changed, the document is gone). A
// quotation checked against a passage that cannot be read is not verified and
// not called wrong either: `verified` stays false and the citation is kept.
// `supplied` is every passage this operation actually supplied to the model.
// A quotation not found in what it cites is looked for there - never in a
// document the operation did not read - and, found, bound to the passage it
// stands in (`rebound`): the model put a true quotation beside the wrong
// marker, and the lawyer is owed the right source, not a warning.
[[nodiscard]] std::vector<QuotedPassage> verify_quotations(const std::string &answer,
	const std::vector<ResolvedSourceReference> &cited,
	const std::function<std::optional<std::string>(const ResolvedSourceReference &)> &passage_text,
	const std::vector<ResolvedSourceReference> &supplied = {});

// `answer` with every verified quotation written exactly as its passage has
// it. Only quotations this same check verified are touched, and only where the
// words already agree: what changes is the kind of quotation mark, a dash, a
// run of spaces or the case of a word - the things that make a quotation look
// verbatim without being it. A quotation that was not verified is left exactly
// as the answer wrote it.
[[nodiscard]] std::string quote_documents_exactly(const std::string &answer,
	const std::vector<QuotedPassage> &checked);

// The quotations of `answer` alone, in the order they stand - exposed for
// tests and for callers that only need to know whether an answer quotes at
// all.
[[nodiscard]] std::vector<std::string> quotations_in(const std::string &answer);

} // namespace singularity::context
