#pragma once

#include "DocumentIntelligence/Types/LegalDocument.hpp"

#include <string>
#include <vector>

// What a change to a clause did, word by word, and what of it its explanation
// leaves unsaid. A colleague answers for the whole of its edit - the words it
// took out as well as the words it put in - so an explanation that leaves an
// edit unsaid is completed from the edit itself, never guessed. And what one
// version of an agreement changes against another, clause by clause: the
// other side's changes in a clean copy.
namespace singularity::document_intelligence {

// One place where a clause changed: the words there before and after it,
// with the few unchanged words between two nearby edits kept inside it, so
// each side reads as a phrase.
struct WordEdit {
	std::string removed;
	std::string added;
	// The words of four letters or more on one side only: what the edit is
	// about, by which an explanation is checked.
	std::vector<std::string> distinctive;
};

// The edits that turn `before` into `after`, word by word, a mark of
// punctuation counting as a word. Empty when they read the same, or when a
// clause is too long to compare word by word (over 1,500 words a side).
std::vector<WordEdit> wordEdits(const std::string& before, const std::string& after);

// A sentence for the edits an explanation does not mention - "It also takes
// out “whatsoever”." - or empty when it mentions every one. An edit is
// mentioned when one of its distinctive words, or a word sharing its first
// six letters, is a word of the explanation; an edit of short words only, when
// one of them is. An edit of punctuation alone is never said.
std::string unsaidEdits(const std::string& explanation, const std::vector<WordEdit>& edits);

// What one clause of an agreement became in another version of it.
struct ClauseChange {
	// The clause in the later version, or in the earlier where it was taken out.
	std::string blockId;
	std::string number;
	std::string heading;
	// "changed", "added" or "removed".
	std::string kind;
	// In words: "adds “whatsoever”", "replaces “five (5)” with “two (2)”", "a
	// new clause: “…”", "taken out; it read “…”".
	std::string description;
};

// The clauses of the agreement's body that `after` changes against `before`,
// matched by their ids (the same clause in every version that numbers it the
// same), in the order `after` reads them, its clauses taken out last. A clause
// whose words differ only in spacing is no change.
std::vector<ClauseChange> clauseChanges(const LegalDocument& before, const LegalDocument& after);

} // namespace singularity::document_intelligence
