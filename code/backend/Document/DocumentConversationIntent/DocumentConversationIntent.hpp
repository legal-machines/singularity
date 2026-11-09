#pragma once

#include <utf8proc.h>

#include <cstdlib>
#include <string>
#include <unordered_set>

namespace singularity::documents {

// Real product bug (caught live): a Project that happens to have an editable
// document made resolve_direct_document_work_task_plan()/prepare_connected_
// document_action() engage for EVERY Chat message, including a bare
// greeting - neither function has ever looked at whether the instruction
// itself expresses any real professional/document intent, only at whether a
// structural target exists (a free FILL_TEMPLATE block, a mutation-capable
// document). A Project having documents must never mean every message is
// about them - Legal task selection does not change this either: "Legal
// task" governs framing/quality, never the actual requested effect.
//
// STATUS UPDATE (model-native ProfessionalExecutionIntent/actionType
// migration, this session): this bypass is NO LONGER what keeps a bare
// greeting from mutating a document - that correctness guarantee now comes
// from the unified professional_response contract itself (Document/
// ProfessionalArtifactAction/ProfessionalArtifactAction.hpp): a structural
// DirectDocumentWork/artifact candidate is always just an OFFERED choice,
// and the model's own actionType ("none" is always legal) is what Singularity
// actually acts on - proven for real, unseen, non-bypass-listed paraphrases
// in ChatExecution.test.cpp's item17. This function now exists PURELY as a
// cost short-circuit: without it, a bare "…" against a Project with a
// large free-template document would still pay for real docx table
// extraction and embed that document's full text into the professional-
// response prompt, only to have the model correctly answer "none" anyway.
// Keep it for that reason - removing it regresses latency/token cost, not
// correctness - but never again read its exact-phrase allowlist as the
// safety boundary; that boundary is the model's own declared actionType.
//
// This is deliberately narrow: it only ever declares the SAFE extreme -
// "this message is plausibly pure social chit-chat, with no professional
// content at all" - by exact match against a bounded stock-phrase list,
// never a general mutation-vs-read-only classifier. Natural language is far
// too varied for a bounded phrase list to answer that broader question
// safely in both directions (see ExternalResearch/ResearchIntent/
// ResearchIntent.hpp's own Required/Off split, which resolves the identical
// tension the same way: declare only the safe extremes by bounded phrase
// list, leave everything else to the existing unconditional resolution
// unchanged). An unlisted way to say hello simply falls through to today's
// behavior - never a new regression; a false positive is not realistic,
// since nobody sends only "…" while actually meaning "add this risk
// to the document."
//
// Called at the top of BOTH resolve_direct_document_work_task_plan() and
// prepare_connected_document_action() - defense in depth, not a single
// upstream gate a future caller could bypass: each document-action entry
// point refuses to engage on its own, regardless of who calls it.
inline bool instruction_bypasses_document_engagement(const std::string &instruction) {
	utf8proc_uint8_t *mapped = nullptr;
	const utf8proc_ssize_t mappedLength = utf8proc_map(
		reinterpret_cast<const utf8proc_uint8_t *>(instruction.data()),
		static_cast<utf8proc_ssize_t>(instruction.size()), &mapped,
		static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD));
	if (mappedLength < 0 || mapped == nullptr) {
		if (mapped != nullptr) std::free(mapped);
		return false;
	}
	const std::string folded(reinterpret_cast<const char *>(mapped), static_cast<std::size_t>(mappedLength));
	std::free(mapped);

	const std::string trimChars = " \t\r\n.,!?;:\xe2\x80\xa6()[]\"'";
	const auto first = folded.find_first_not_of(trimChars);
	if (first == std::string::npos) return true; // Blank/punctuation-only never engages document work either.
	const auto last = folded.find_last_not_of(trimChars);
	const std::string trimmed = folded.substr(first, last - first + 1);

	static const std::unordered_set<std::string> conversationalPhrases = {
		"\320\277\321\200\320\270\320\262\320\265\321\202", "\320\277\321\200\320\270\320\262\320\265\321\202\320\270\320\272", "\320\267\320\264\321\200\320\260\320\262\321\201\321\202\320\262\321\203\320\271", "\320\267\320\264\321\200\320\260\320\262\321\201\321\202\320\262\321\203\320\271\321\202\320\265", "\320\264\320\276\320\261\321\200\321\213\320\271 \320\264\320\265\320\275\321\214", "\320\264\320\276\320\261\321\200\321\213\320\271 \320\262\320\265\321\207\320\265\321\200", "\320\264\320\276\320\261\321\200\320\276\320\265 \321\203\321\202\321\200\320\276",
		"\320\267\320\264\320\276\321\200\320\276\320\262\320\276", "\321\205\320\260\320\271", "\320\272\321\203", "\321\201\320\277\320\260\321\201\320\270\320\261\320\276", "\321\201\320\277\320\260\321\201\320\270\320\261\320\276 \320\261\320\276\320\273\321\214\321\210\320\276\320\265", "\320\261\320\273\320\260\320\263\320\276\320\264\320\260\321\200\321\216", "\320\277\320\276\320\266\320\260\320\273\321\203\320\271\321\201\321\202\320\260",
		"\320\276\320\272", "\320\276\320\272\320\265\320\271", "\321\205\320\276\321\200\320\276\321\210\320\276", "\320\277\320\276\320\275\321\217\321\202\320\275\320\276", "\321\217\321\201\320\275\320\276", "\321\201\321\203\320\277\320\265\321\200", "\320\272\320\273\320\260\321\201\321\201", "\320\276\321\202\320\273\320\270\321\207\320\275\320\276",
		"\320\277\320\276\320\272\320\260", "\320\264\320\276 \321\201\320\262\320\270\320\264\320\260\320\275\320\270\321\217", "\320\264\320\276 \320\262\321\201\321\202\321\200\320\265\321\207\320\270", "\320\272\320\260\320\272 \320\264\320\265\320\273\320\260", "\320\272\320\260\320\272 \321\202\321\213", "\320\272\320\260\320\272 \320\266\320\270\320\267\320\275\321\214",
		"hi", "hello", "hey", "yo", "good morning", "good afternoon", "good evening",
		"thanks", "thank you", "thanks a lot", "ok", "okay", "cool", "great", "nice", "got it",
		"sounds good", "bye", "goodbye", "see you", "how are you", "what's up",
		// Meta/product questions about Singularity itself - the same SAFE-extreme
		// exact-match technique, extended to a second bounded category (Beta
		// UX correction: "…" was reaching the full document-
		// mutation/ProfessionalReview machinery and throwing
		// professional_review_exhausted for a question that has nothing to
		// do with any Project document).
		"\320\272\320\260\320\272\320\260\321\217 \321\202\321\213 \320\274\320\276\320\264\320\265\320\273\321\214", "\320\272\320\260\320\272\321\203\321\216 \320\274\320\276\320\264\320\265\320\273\321\214 \321\202\321\213 \320\270\321\201\320\277\320\276\320\273\321\214\320\267\321\203\320\265\321\210\321\214", "\321\202\321\213 \320\272\320\260\320\272\320\260\321\217 \320\274\320\276\320\264\320\265\320\273\321\214", "\320\272\320\260\320\272\320\260\321\217 \320\274\320\276\320\264\320\265\320\273\321\214",
		"\321\202\321\213 \320\273\320\276\320\272\320\260\320\273\321\214\320\275\320\260\321\217 \320\274\320\276\320\264\320\265\320\273\321\214", "\321\202\321\213 \320\273\320\276\320\272\320\260\320\273\321\214\320\275\320\260\321\217 \320\270\320\273\320\270 \320\276\320\261\320\273\320\260\321\207\320\275\320\260\321\217", "\320\273\320\276\320\272\320\260\320\273\321\214\320\275\320\260\321\217 \320\274\320\276\320\264\320\265\320\273\321\214 \320\270\320\273\320\270 \320\276\320\261\320\273\320\260\321\207\320\275\320\260\321\217",
		"\321\207\321\202\320\276 \321\202\321\213 \321\203\320\274\320\265\320\265\321\210\321\214", "\321\207\321\202\320\276 \321\203\320\274\320\265\320\265\321\210\321\214", "\321\207\320\265\320\274 \321\202\321\213 \320\274\320\276\320\266\320\265\321\210\321\214 \320\277\320\276\320\274\320\276\321\207\321\214", "\320\277\321\200\320\276\320\264\320\276\320\273\320\266\320\270\320\274",
		"what model are you", "which model are you", "what model is this", "are you local or cloud",
		"what can you do", "what do you do", "shall we continue", "let's continue",
	};
	return conversationalPhrases.count(trimmed) > 0;
}

} // namespace singularity::documents
