#include "Context/TemporalContext/TemporalContext.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace singularity::context::temporal_context {
namespace {

using temporal::TemporalOrder;
using temporal::TemporalValue;

// Lower case for ASCII and Cyrillic, byte for byte the same length.
std::string lowered(const std::string &text) {
	std::string result = text;
	for (std::size_t index = 0; index < result.size(); ++index) {
		const unsigned char byte = static_cast<unsigned char>(result[index]);
		if (byte >= 'A' && byte <= 'Z') {
			result[index] = static_cast<char>(byte - 'A' + 'a');
		} else if (byte == 0xD0 && index + 1 < result.size()) {
			const unsigned char next = static_cast<unsigned char>(result[index + 1]);
			if (next >= 0x90 && next <= 0x9F) result[index + 1] = static_cast<char>(next + 0x20);
			else if (next >= 0xA0 && next <= 0xAF) { result[index] = static_cast<char>(0xD1); result[index + 1] = static_cast<char>(next - 0x20); }
			else if (next == 0x81) { result[index] = static_cast<char>(0xD1); result[index + 1] = static_cast<char>(0x91); }
			++index;
		} else if (byte >= 0xC0) {
			++index;
		}
	}
	return result;
}

bool letter_at(const std::string &text, std::size_t position) {
	if (position >= text.size()) return false;
	const unsigned char byte = static_cast<unsigned char>(text[position]);
	return std::isalpha(byte) != 0 || byte == 0xD0 || byte == 0xD1;
}

// `word` stands in `text` as a word of its own - or, for a Russian stem, with an
// ending of at most `ending` bytes after it.
bool has_word(const std::string &text, const std::string &word, std::size_t ending = 0) {
	for (std::size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
		if (at > 0) {
			const unsigned char before = static_cast<unsigned char>(text[at - 1]);
			// A Cyrillic letter's second byte, or any letter, before it: inside a word.
			if (std::isalpha(before) != 0 || (before >= 0x80 && before <= 0xBF && at >= 2
					&& (static_cast<unsigned char>(text[at - 2]) == 0xD0 || static_cast<unsigned char>(text[at - 2]) == 0xD1)))
				continue;
		}
		std::size_t after = at + word.size();
		std::size_t extra = 0;
		while (extra < ending && letter_at(text, after)) {
			const unsigned char byte = static_cast<unsigned char>(text[after]);
			after += byte == 0xD0 || byte == 0xD1 ? 2 : 1;
			extra += byte == 0xD0 || byte == 0xD1 ? 2 : 1;
		}
		if (!letter_at(text, after)) return true;
	}
	return false;
}

bool has_any(const std::string &text, const std::vector<std::string> &words, std::size_t ending = 0) {
	return std::any_of(words.begin(), words.end(), [&](const std::string &word) { return has_word(text, word, ending); });
}

bool contains(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

// The document's first lines that carry words - its title, and what dates it.
std::vector<std::string> head_lines(const std::string &text, std::size_t count) {
	std::vector<std::string> lines;
	std::size_t at = 0;
	while (at < text.size() && lines.size() < count) {
		const std::size_t end = text.find('\n', at);
		std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
		const bool words = std::any_of(line.begin(), line.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0
			|| static_cast<unsigned char>(c) >= 0x80; });
		if (words) lines.push_back(std::move(line));
		if (end == std::string::npos) break;
		at = end + 1;
	}
	return lines;
}

// Cue words, English and Russian, lower case. Russian stems allow an ending.
const std::vector<std::string> kDraftWords{"draft", "drafts", "for discussion"};
const std::vector<std::string> kDraftStems{"\xD1\x87\xD0\xB5\xD1\x80\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xB8\xD0\xBA"};  // …

// «…» marks a draft only as the word itself where a title or a name puts
// it - first («…»), last («…»),
// or before the paper it is a draft of («…») - never as the
// stem of «…» or «…», the subject of an
// executed design or investment contract.
bool russian_draft_word(const std::string &lower) {
	static const std::string project = "\xD0\xBF\xD1\x80\xD0\xBE\xD0\xB5\xD0\xBA\xD1\x82";  // …
	static const std::vector<std::string> papers{
		"\xD0\xB4\xD0\xBE\xD0\xB3\xD0\xBE\xD0\xB2\xD0\xBE\xD1\x80\xD0\xB0",                     // …
		"\xD1\x81\xD0\xBE\xD0\xB3\xD0\xBB\xD0\xB0\xD1\x88\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F",       // …
		"\xD0\xBA\xD0\xBE\xD0\xBD\xD1\x82\xD1\x80\xD0\xB0\xD0\xBA\xD1\x82\xD0\xB0",             // …
		"\xD0\xB4\xD0\xBE\xD0\xBF\xD0\xBE\xD0\xBB\xD0\xBD\xD0\xB8\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xBE\xD0\xB3\xD0\xBE",  // …
		"\xD0\xBF\xD1\x80\xD0\xBE\xD1\x82\xD0\xBE\xD0\xBA\xD0\xBE\xD0\xBB\xD0\xB0",             // …
		"\xD1\x80\xD0\xB5\xD1\x88\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F",                               // …
		"\xD0\xB0\xD0\xBA\xD1\x82\xD0\xB0",                                                           // …
		"\xD1\x83\xD1\x81\xD1\x82\xD0\xB0\xD0\xB2\xD0\xB0",                                         // …
		"\xD0\xB4\xD0\xBE\xD0\xBA\xD1\x83\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x82\xD0\xB0",             // …
		"\xD0\xB8\xD0\xB7\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB9",             // …
		"\xD0\xBF\xD0\xB8\xD1\x81\xD1\x8C\xD0\xBC\xD0\xB0",                                         // …
		"\xD0\xBF\xD1\x80\xD0\xB8\xD0\xBA\xD0\xB0\xD0\xB7\xD0\xB0"};                               // …
	std::vector<std::string> words;
	std::string word;
	for (std::size_t index = 0; index <= lower.size(); ++index) {
		if (index < lower.size() && letter_at(lower, index)) {
			const std::size_t size = static_cast<unsigned char>(lower[index]) >= 0xC0 ? 2 : 1;
			word += lower.substr(index, size);
			index += size - 1;
		} else if (!word.empty()) {
			words.push_back(word);
			word.clear();
		}
	}
	for (std::size_t index = 0; index < words.size(); ++index) {
		if (words[index] != project) continue;
		if (index == 0 || index + 1 == words.size()) return true;
		if (std::find(papers.begin(), papers.end(), words[index + 1]) != papers.end()) return true;
	}
	return false;
}

// A signature given, as a text shows one: "/s/ Dana Morgan", "By: Dana Morgan"
// (never "By: ______" or "By:" with "Name:" under it), a platform's or a
// qualified electronic signature's mark ("DocuSign Envelope ID", «…
// …», «…»), or a «…» line that
// carries a date or a name and no line left to sign on.
bool shows_signature(const std::string &lower) {
	for (std::size_t at = lower.find("/s/"); at != std::string::npos; at = lower.find("/s/", at + 1)) {
		std::size_t next = at + 3;
		while (next < lower.size() && lower[next] == ' ') ++next;
		if (next < lower.size() && (letter_at(lower, next) || lower[next] == '[')) return true;
	}
	for (std::size_t at = lower.find("by:"); at != std::string::npos; at = lower.find("by:", at + 1)) {
		if (at > 0 && letter_at(lower, at - 1)) continue;
		std::size_t next = at + 3;
		while (next < lower.size() && (lower[next] == ' ' || lower[next] == '\t')) ++next;
		if (next >= lower.size() || !(letter_at(lower, next) || lower[next] == '[')) continue;
		const std::string following = lower.substr(next, 6);
		if (following.rfind("name", 0) == 0 || following.rfind("title", 0) == 0 || following.rfind("its", 0) == 0
				|| following.rfind("[\xE2", 0) == 0 || following.rfind("[_", 0) == 0 || following.rfind("[sig", 0) == 0
				|| following.rfind("[name", 0) == 0)
			continue;
		return true;
	}
	if (contains(lower, "docusign envelope id") || contains(lower, "electronically signed") || contains(lower, "digitally signed")
			|| contains(lower, "signed electronically"))
		return true;
	static const std::vector<std::string> russianMarks{
		"\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB0\xD0\xBD\xD0\xBE",      // …
		"\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB0\xD0\xBD",                // …
		"\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB0\xD0\xBD\xD0\xB0"};     // …
	if (has_any(lower, russianMarks)) return true;
	// «…» - a «…» line with a date, and no line to sign on.
	static const std::string signature = "\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBF\xD0\xB8\xD1\x81\xD1\x8C";  // …
	for (std::size_t at = lower.find(signature); at != std::string::npos; at = lower.find(signature, at + 1)) {
		const std::size_t lineStart = at == 0 ? 0 : (lower.rfind('\n', at - 1) == std::string::npos ? 0 : lower.rfind('\n', at - 1) + 1);
		const std::size_t lineEnd = std::min(lower.find('\n', at), lower.size());
		const std::string line = lower.substr(lineStart, lineEnd - lineStart);
		if (contains(line, "__") || contains(line, "\xE2\x80\xA6")) continue;
		if (!temporal::find(line).empty()) return true;
	}
	return false;
}
// A text in final form: every line to sign on names its signatory -
// «…», "By: ________ Name: Dana Morgan" - and no
// blank is left anywhere: no «___» day, no «/______/» for a name, no [●]. A
// draft leaves them blank; a text never shows a handwritten signature, so an
// executed Russian agreement kept in Word reads exactly like this (live Sol
// runs, 27 Sep 2026: calling such agreements ones whose text "does not show
// whether it was signed" made every answer doubt the terms the matter runs on).
bool final_form(const std::string &lower) {
	for (const char *blank : {"[\xE2\x97\x8F]", "[\xE2\x80\xA2]", "[__", "\xC2\xAB__", "[date", "[\xD0\xB4\xD0\xB0\xD1\x82\xD0\xB0",
			"[\xD0\xBC\xD0\xB5\xD1\x81\xD1\x82\xD0\xBE", "[\xD0\xB3\xD0\xBE\xD1\x80\xD0\xBE\xD0\xB4"})
		if (contains(lower, blank)) return false;
	bool lineToSign = false;
	std::size_t from = 0;
	while (from < lower.size()) {
		const std::size_t end = std::min(lower.find('\n', from), lower.size());
		const std::string line = lower.substr(from, end - from);
		from = end + 1;
		const std::size_t run = line.rfind("____");
		if (run == std::string::npos) continue;
		lineToSign = true;
		// What follows the last line to sign on, labels aside, must be a name.
		std::size_t after = run;
		while (after < line.size() && line[after] == '_') ++after;
		std::string rest = line.substr(after);
		for (const std::string label : {"name:", "title:", "its:"})
			for (std::size_t at = rest.find(label); at != std::string::npos; at = rest.find(label))
				rest.erase(at, label.size());
		bool named = false;
		for (std::size_t index = 0; index < rest.size() && !named; ++index) named = letter_at(rest, index);
		if (!named) return false;
	}
	return lineToSign;
}
const std::vector<std::string> kAmendmentWords{"amendment", "side letter", "addendum", "supplement"};
const std::vector<std::string> kAmendmentStems{"\xD0\xB4\xD0\xBE\xD0\xBF\xD0\xBE\xD0\xBB\xD0\xBD\xD0\xB8\xD1\x82\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xBD", // …(…)
	"\xD0\xB4\xD0\xBE\xD0\xBF\xD1\x81\xD0\xBE\xD0\xB3\xD0\xBB\xD0\xB0\xD1\x88\xD0\xB5\xD0\xBD"};                // …
const std::vector<std::string> kBoardWords{"minutes", "board of directors", "resolution", "resolutions", "written consent"};
const std::vector<std::string> kBoardStems{"\xD0\xBF\xD1\x80\xD0\xBE\xD1\x82\xD0\xBE\xD0\xBA\xD0\xBE\xD0\xBB", // …
	"\xD1\x81\xD0\xBE\xD0\xB2\xD0\xB5\xD1\x82 \xD0\xB4\xD0\xB8\xD1\x80\xD0\xB5\xD0\xBA\xD1\x82\xD0\xBE\xD1\x80\xD0\xBE\xD0\xB2", // …
	"\xD0\xBE\xD0\xB1\xD1\x89\xD0\xB5\xD0\xB5 \xD1\x81\xD0\xBE\xD0\xB1\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5"};  // …
const std::vector<std::string> kCorrespondenceWords{"letter", "notice", "claim", "demand", "correspondence", "email"};
const std::vector<std::string> kCorrespondenceStems{"\xD0\xBF\xD1\x80\xD0\xB5\xD1\x82\xD0\xB5\xD0\xBD\xD0\xB7\xD0\xB8", // …
	"\xD0\xBF\xD0\xB8\xD1\x81\xD1\x8C\xD0\xBC",                                                                   // …
	"\xD1\x83\xD0\xB2\xD0\xB5\xD0\xB4\xD0\xBE\xD0\xBC\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xB8",                           // …
	"\xD0\xBF\xD0\xB5\xD1\x80\xD0\xB5\xD0\xBF\xD0\xB8\xD1\x81\xD0\xBA"};                                          // …
const std::vector<std::string> kNoteWords{"notes", "note", "memo", "memorandum"};
const std::vector<std::string> kNoteStems{"\xD0\xB7\xD0\xB0\xD0\xBF\xD0\xB8\xD1\x81\xD0\xBA",                 // …(…)
	"\xD0\xB7\xD0\xB0\xD0\xBC\xD0\xB5\xD1\x82\xD0\xBA"};                                                          // …(…)
const std::vector<std::string> kRegisterWords{"register", "schedule", "tracker", "log"};
const std::vector<std::string> kRegisterStems{"\xD1\x80\xD0\xB5\xD0\xB5\xD1\x81\xD1\x82\xD1\x80"};            // …
const std::vector<std::string> kAgreementWords{"agreement", "contract"};
const std::vector<std::string> kAgreementStems{"\xD0\xB4\xD0\xBE\xD0\xB3\xD0\xBE\xD0\xB2\xD0\xBE\xD1\x80",     // …
	"\xD1\x81\xD0\xBE\xD0\xB3\xD0\xBB\xD0\xB0\xD1\x88\xD0\xB5\xD0\xBD\xD0\xB8"};                                  // …

bool cued(const std::string &text, const std::vector<std::string> &words, const std::vector<std::string> &stems) {
	return has_any(text, words) || has_any(text, stems, 8);
}

Standing standing_of(const std::string &path, const std::string &title, const std::string &text, const std::string &extension) {
	const std::string where = lowered(path);
	const std::string heading = lowered(title);
	const std::string body = lowered(text);
	// A heading or opening with blanks still to fill - "No. [●]", «№ [•]»,
	// «…», ««___» ________ 2026 …» - is a draft, however its signature
	// lines look: a draft has lines to sign on too (a live run read Singularity's
	// own unsigned additional agreement as signed, 24 Sep 2026).
	const std::string opening = body.substr(0, std::min<std::size_t>(body.size(), 600));
	const bool blanks = contains(opening, "[\xE2\x97\x8F]") || contains(opening, "[\xE2\x80\xA2]")
		|| contains(opening, "[__") || contains(opening, "\xC2\xAB___") || contains(opening, "\xC2\xAB__\xC2\xBB")
		|| contains(opening, "[date") || contains(opening, "[\xD0\xB4\xD0\xB0\xD1\x82\xD0\xB0")   // […
		|| contains(opening, "[\xD0\xBC\xD0\xB5\xD1\x81\xD1\x82\xD0\xBE")                         // […
		|| contains(opening, "[\xD0\xB3\xD0\xBE\xD1\x80\xD0\xBE\xD0\xB4");                        // […
	// A folder of drafts («…», "Drafts") or the word in a title or a name.
	bool draftFolder = false;
	{
		std::size_t from = 0;
		for (std::size_t slash = where.find('/'); slash != std::string::npos; slash = where.find('/', from)) {
			const std::string folder = where.substr(from, slash - from);
			if (folder == "\xD0\xBF\xD1\x80\xD0\xBE\xD0\xB5\xD0\xBA\xD1\x82\xD1\x8B"            // …
					|| folder == "\xD0\xBF\xD1\x80\xD0\xBE\xD0\xB5\xD0\xBA\xD1\x82")               // …
				draftFolder = true;
			from = slash + 1;
		}
	}
	const std::string fileName = where.substr(where.rfind('/') == std::string::npos ? 0 : where.rfind('/') + 1);
	const std::string fileStem = fileName.substr(0, fileName.rfind('.'));
	std::string nameWords = fileStem;
	std::replace(nameWords.begin(), nameWords.end(), '_', ' ');
	const bool draft = cued(where, kDraftWords, kDraftStems) || cued(heading, kDraftWords, kDraftStems) || blanks || draftFolder
		|| russian_draft_word(nameWords);
	const bool headingDraft = [&] {
		std::size_t from = 0;
		while (from < heading.size()) {
			const std::size_t end = std::min(heading.find('\n', from), heading.size());
			if (russian_draft_word(heading.substr(from, end - from))) return true;
			from = end + 1;
		}
		return false;
	}();
	const bool isDraft = draft || headingDraft;
	// A signature given - never a line left to sign on, which drafts have too.
	const bool signedMarks = shows_signature(body);
	const bool finalForm = !isDraft && !signedMarks && final_form(body);
	const std::string named = where + "\n" + heading;
	if (cued(named, kAmendmentWords, kAmendmentStems))
		return isDraft ? Standing::DraftAmendment : signedMarks ? Standing::SignedAmendment
			: finalForm ? Standing::FinalAmendment : Standing::Amendment;
	if (cued(named, kBoardWords, kBoardStems)) return Standing::BoardRecord;
	if (extension == ".xlsx" || extension == ".xls" || extension == ".csv" || extension == ".tsv"
			|| cued(named, kRegisterWords, kRegisterStems))
		return Standing::Register;
	if (cued(named, kCorrespondenceWords, kCorrespondenceStems)) return Standing::Correspondence;
	if (extension == ".md" || cued(named, kNoteWords, kNoteStems)) return Standing::Note;
	if (cued(named, kAgreementWords, kAgreementStems)) {
		if (isDraft) return Standing::Draft;
		return signedMarks ? Standing::Signed : finalForm ? Standing::Final : Standing::Agreement;
	}
	return Standing::Unknown;
}

// The date of ANOTHER document the text refers to - «… № …
// … «03» … 2026 …», «…», "to the Master Services
// Agreement dated March 3, 2026" - never dates this one: an amendment, a letter
// or minutes that name the agreement they concern would otherwise take its
// date (live Russian run, 24 Sep 2026: minutes of 14.08.2026 dated 03.03.2026).
bool refers_to_another_document(const std::string &lowerText, std::size_t offset) {
	std::size_t from = offset > 200 ? offset - 200 : 0;
	const std::size_t lineStart = offset == 0 ? std::string::npos : lowerText.rfind('\n', offset - 1);
	if (lineStart != std::string::npos && lineStart + 1 > from) from = lineStart + 1;
	const std::string before = lowerText.substr(from, offset - from);
	// Its agreement named in a case that points away from this document (Russian),
	// or after a preposition that does (English).
	static const std::vector<std::string> obliqueRu{
		"\xD0\xB4\xD0\xBE\xD0\xB3\xD0\xBE\xD0\xB2\xD0\xBE\xD1\x80\xD0\xB0",           // …
		"\xD0\xB4\xD0\xBE\xD0\xB3\xD0\xBE\xD0\xB2\xD0\xBE\xD1\x80\xD1\x83",           // …
		"\xD0\xB4\xD0\xBE\xD0\xB3\xD0\xBE\xD0\xB2\xD0\xBE\xD1\x80\xD0\xBE\xD0\xBC",   // …
		"\xD1\x81\xD0\xBE\xD0\xB3\xD0\xBB\xD0\xB0\xD1\x88\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F", // …
		"\xD1\x81\xD0\xBE\xD0\xB3\xD0\xBB\xD0\xB0\xD1\x88\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8E", // …
		"\xD0\xBA\xD0\xBE\xD0\xBD\xD1\x82\xD1\x80\xD0\xB0\xD0\xBA\xD1\x82\xD0\xB0",   // …
		"\xD0\xBA\xD0\xBE\xD0\xBD\xD1\x82\xD1\x80\xD0\xB0\xD0\xBA\xD1\x82\xD1\x83"};  // …
	for (const std::string &form : obliqueRu)
		if (before.find(form) != std::string::npos) return true;
	for (const std::string &word : {std::string("agreement"), std::string("contract")}) {
		const std::size_t at = before.rfind(word);
		if (at == std::string::npos) continue;
		const std::size_t reach = at > 60 ? at - 60 : 0;
		const std::string lead = before.substr(reach, at - reach);
		for (const std::string &preposition : {std::string("to the "), std::string("under the "), std::string("of the "),
				std::string("to that "), std::string("amending the ")})
			if (lead.find(preposition) != std::string::npos) return true;
	}
	return false;
}

// Words that date a document, right before its date: "entered into as of",
// "dated", "Minutes ... -", «…».
bool dating_cue_before(const std::string &lowerText, std::size_t offset) {
	if (refers_to_another_document(lowerText, offset)) return false;
	const std::size_t from = offset > 48 ? offset - 48 : 0;
	const std::string before = lowerText.substr(from, offset - from);
	static const std::vector<std::string> cues{"as of", "as at", "dated", "entered into on", "made on", "effective", "date:",
		"updated", "\xD0\xBE\xD1\x82", "\xD0\xB4\xD0\xB0\xD1\x82\xD0\xB0",
		"\xD0\xBF\xD0\xBE \xD1\x81\xD0\xBE\xD1\x81\xD1\x82\xD0\xBE\xD1\x8F\xD0\xBD\xD0\xB8\xD1\x8E \xD0\xBD\xD0\xB0", // …
		"\xD0\xBE\xD0\xB1\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xBE"};                           // …
	for (const std::string &cue : cues) {
		const std::size_t at = before.rfind(cue);
		if (at == std::string::npos) continue;
		// Nothing but spaces, quotes and punctuation between the cue and the date.
		const std::string gap = before.substr(at + cue.size());
		if (std::all_of(gap.begin(), gap.end(), [](char c) {
				const unsigned char byte = static_cast<unsigned char>(c);
				return byte == ' ' || byte == ':' || byte == ',' || byte == '"' || byte == '\'' || byte == 0xC2 || byte == 0xAB
					|| byte == 0xBB || byte == 0xA0;
			}))
			return true;
	}
	return false;
}

bool precise_enough(const TemporalValue &value) {
	return value.precision == temporal::Precision::Day || value.precision == temporal::Precision::Month;
}

// The date the document's own text gives it: a date that follows a dating
// word in its opening, or else one in its title.
// A slashed day that the document's own dates leave open ("05/06/2026" in a
// text that shows neither order) dates nothing: it is May 6 or 5 June.
std::optional<std::pair<TemporalValue, std::string>> text_date(const std::string &text, temporal::SlashOrder order) {
	const std::string opening = text.substr(0, std::min<std::size_t>(text.size(), 3000));
	const std::string lowerOpening = lowered(opening);
	const auto found = temporal::find(opening, order);
	for (const auto &date : found)
		if (!date.ambiguous && precise_enough(date.value) && dating_cue_before(lowerOpening, date.offset))
			return std::make_pair(date.value, opening.substr(date.offset, date.length));
	const std::vector<std::string> title = head_lines(text, 2);
	for (const std::string &line : title) {
		const auto inTitle = temporal::find(line, order);
		const std::string lowerLine = lowered(line);
		for (const auto &date : inTitle)
			if (!date.ambiguous && precise_enough(date.value) && !refers_to_another_document(lowerLine, date.offset))
				return std::make_pair(date.value, line.substr(date.offset, date.length));
	}
	// The line a Russian document is dated on: «…» 2026 …»,
	// the place and then the date, among its first lines.
	for (const std::string &written : head_lines(text, 6)) {
		const std::size_t start = written.find_first_not_of(" \t");
		if (start == std::string::npos) continue;
		const std::string line = written.substr(start);
		if (line.rfind("\xD0\xB3.", 0) != 0) continue;   // …
		for (const auto &date : temporal::find(line, order))
			if (!date.ambiguous && precise_enough(date.value)) return std::make_pair(date.value, line.substr(date.offset, date.length));
	}
	return std::nullopt;
}

std::optional<std::pair<TemporalValue, std::string>> name_date(const std::string &relative_path, temporal::SlashOrder order) {
	const std::string stem = std::filesystem::path(relative_path).stem().u8string();
	for (const auto &date : temporal::find(stem, order))
		if (!date.ambiguous && precise_enough(date.value)) return std::make_pair(date.value, stem.substr(date.offset, date.length));
	return std::nullopt;
}

// What a document is an instance of: its title's words, without its status,
// number or dates - "master services agreement".
std::string family_of(const std::string &text) {
	for (const std::string &line : head_lines(text, 4)) {
		std::string lower = lowered(line);
		if (!cued(lower, kAgreementWords, kAgreementStems) && !cued(lower, kAmendmentWords, kAmendmentStems)) continue;
		// Dates and what follows a number sign go.
		for (const auto &date : temporal::find(lower)) std::fill(lower.begin() + date.offset, lower.begin() + date.offset + date.length, ' ');
		const std::size_t number = lower.find("\xE2\x84\x96");
		if (number != std::string::npos) lower = lower.substr(0, number);
		const std::size_t parenthesis = lower.find('(');
		if (parenthesis != std::string::npos) lower = lower.substr(0, parenthesis);
		std::string family;
		bool space = false;
		for (std::size_t index = 0; index < lower.size(); ++index) {
			const unsigned char byte = static_cast<unsigned char>(lower[index]);
			if (std::isalpha(byte) != 0 || byte >= 0x80) {
				if (space && !family.empty()) family.push_back(' ');
				space = false;
				family.push_back(lower[index]);
			} else {
				space = true;
			}
		}
		for (const std::string &status : {std::string("draft"), std::string("executed"),
				std::string("\xD0\xBF\xD1\x80\xD0\xBE\xD0\xB5\xD0\xBA\xD1\x82")}) {
			const std::size_t at = family.find(status);
			if (at == 0) family = family.substr(std::min(family.size(), status.size() + 1));
		}
		return family;
	}
	return {};
}

const char *standing_words(Standing standing) {
	switch (standing) {
		case Standing::Draft: return "a draft agreement, not signed";
		case Standing::Signed: return "the signed agreement";
		case Standing::Final: return "an agreement in final form: every signatory named, nothing left blank";
		case Standing::Agreement: return "an agreement whose text does not show whether it was signed";
		case Standing::DraftAmendment: return "a draft amendment, not signed";
		case Standing::Amendment: return "an amendment or side letter whose text does not show whether it was signed";
		case Standing::SignedAmendment: return "a signed amendment or side letter";
		case Standing::FinalAmendment: return "an amendment or side letter in final form: every signatory named, nothing left blank";
		case Standing::BoardRecord: return "a board or shareholder record";
		case Standing::Correspondence: return "correspondence";
		case Standing::Note: return "a note or memo";
		case Standing::Register: return "a register or schedule";
		case Standing::Unknown: break;
	}
	return "a document";
}

struct Placed {
	const SuppliedDocument *document = nullptr;
	DocumentDating dating;
};

std::vector<DatingEvidence> evidence_of(const Placed &first, const Placed &second) {
	std::vector<DatingEvidence> evidence = first.dating.evidence;
	evidence.insert(evidence.end(), second.dating.evidence.begin(), second.dating.evidence.end());
	return evidence;
}

} // namespace

DocumentDating date_document(const SuppliedDocument &document) {
	DocumentDating dating;
	const std::string extension = lowered(std::filesystem::path(document.relative_path).extension().u8string());
	// Its title: the opening lines short enough to be one - never the preamble
	// ("It supersedes the draft of ..." does not make a signed agreement a draft).
	// Short enough counts letters, not bytes: a Cyrillic letter is two bytes, and
	// is a title.
	std::string heading;
	for (const std::string &line : head_lines(document.text, 3)) {
		const std::size_t letters = static_cast<std::size_t>(std::count_if(line.begin(), line.end(),
			[](char byte) { return (static_cast<unsigned char>(byte) & 0xC0) != 0x80; }));
		if (letters <= 90) heading += line + "\n";
	}
	dating.standing = standing_of(document.relative_path, heading, document.text, extension);
	dating.family = family_of(document.text);
	// The document's slashes are read in the order its own dates show.
	const temporal::SlashOrder order = temporal::slash_order_of(document.text);
	const auto fromText = text_date(document.text, order);
	const auto fromName = name_date(document.relative_path, order);
	if (fromText) dating.evidence.push_back({"text", fromText->second});
	if (fromName) dating.evidence.push_back({"name", fromName->second});
	if (fromText && fromName) {
		const TemporalOrder order = temporal::compare(fromText->first, fromName->first);
		if (order == TemporalOrder::Before || order == TemporalOrder::After) {
			// Two dates for one document: neither is chosen.
			dating.conflicting = true;
			return dating;
		}
		// The same date, or one within the other: the more precise.
		const bool textFiner = (fromText->first.last_day - fromText->first.first_day)
			<= (fromName->first.last_day - fromName->first.first_day);
		dating.date = textFiner ? fromText->first : fromName->first;
		return dating;
	}
	if (fromText) dating.date = fromText->first;
	else if (fromName) dating.date = fromName->first;
	return dating;
}

TemporalContextResult derive(const std::vector<SuppliedDocument> &documents) {
	return derive(date_documents(documents));
}

std::vector<DatedDocument> date_documents(const std::vector<SuppliedDocument> &documents) {
	std::vector<DatedDocument> dated;
	dated.reserve(documents.size());
	for (const SuppliedDocument &document : documents) dated.push_back({document, date_document(document)});
	return dated;
}

TemporalContextResult derive(const std::vector<DatedDocument> &documents) {
	TemporalContextResult result;
	std::vector<Placed> placed;
	std::vector<Placed> conflicting;
	// What an undated agreement or amendment is still tells a draft from the
	// agreement («___» … 2026 … dates nothing, and is the mark of a draft).
	std::vector<Placed> undated;
	for (const DatedDocument &dated : documents) {
		Placed entry{&dated.document, dated.dating};
		if (entry.dating.standing == Standing::Unknown) continue;
		if (entry.dating.conflicting) conflicting.push_back(std::move(entry));
		else if (entry.dating.date) placed.push_back(std::move(entry));
		else if (entry.dating.standing != Standing::BoardRecord && entry.dating.standing != Standing::Correspondence
				&& entry.dating.standing != Standing::Note && entry.dating.standing != Standing::Register)
			undated.push_back(std::move(entry));
	}
	if (placed.size() + conflicting.size() < 2 || placed.empty()) return result;
	// Earliest first; the order they were supplied in never matters.
	std::sort(placed.begin(), placed.end(), [](const Placed &left, const Placed &right) {
		if (left.dating.date->first_day != right.dating.date->first_day) return left.dating.date->first_day < right.dating.date->first_day;
		if (left.dating.date->last_day != right.dating.date->last_day) return left.dating.date->last_day < right.dating.date->last_day;
		return left.document->document_id < right.document->document_id;
	});
	std::sort(conflicting.begin(), conflicting.end(), [](const Placed &left, const Placed &right) {
		return left.document->document_id < right.document->document_id;
	});

	std::vector<std::string> lines;
	{
		std::string what = "What each document is:";
		std::vector<const Placed *> all;
		for (const Placed &entry : placed) all.push_back(&entry);
		for (const Placed &entry : conflicting) all.push_back(&entry);
		for (const Placed &entry : undated) all.push_back(&entry);
		std::sort(all.begin(), all.end(), [](const Placed *left, const Placed *right) {
			return left->document->handle < right->document->handle;
		});
		bool first = true;
		for (const Placed *entry : all) {
			what += std::string(first ? " " : "; ") + entry->document->handle + " is " + standing_words(entry->dating.standing)
				+ (entry->dating.date || entry->dating.conflicting ? "" : " (undated)");
			first = false;
		}
		lines.push_back(what + ".");
	}
	const auto relate = [&](const std::string &statement, const Placed &first, const Placed &second) {
		lines.push_back(statement);
		result.relations.push_back({statement, {first.document->document_id, second.document->document_id}, evidence_of(first, second)});
	};
	// Each document against the next one that can be told apart from it: a
	// chain, not every pair - the order of the whole follows from it.
	for (std::size_t index = 0; index + 1 < placed.size(); ++index) {
		const Placed &earlier = placed[index];
		for (std::size_t next = index + 1; next < placed.size(); ++next) {
			const Placed &later = placed[next];
			const TemporalOrder order = temporal::compare(*earlier.dating.date, *later.dating.date);
			if (order == TemporalOrder::Before) {
				relate(earlier.document->handle + " precedes " + later.document->handle + ".", earlier, later);
				break;
			}
			if (order == TemporalOrder::Same) {
				relate(earlier.document->handle + " and " + later.document->handle + " are dated the same day.", earlier, later);
				break;
			}
			relate("The order of " + earlier.document->handle + " and " + later.document->handle
				+ " is not known from their dates.", earlier, later);
		}
	}
	// A draft and the signed or final agreement it became, or a draft made after it.
	for (const Placed &draft : placed) {
		if (draft.dating.standing != Standing::Draft || draft.dating.family.empty()) continue;
		for (const Placed &signedOne : placed) {
			const bool isSigned = signedOne.dating.standing == Standing::Signed;
			if ((!isSigned && signedOne.dating.standing != Standing::Final) || signedOne.dating.family != draft.dating.family)
				continue;
			const TemporalOrder order = temporal::compare(*draft.dating.date, *signedOne.dating.date);
			if (order == TemporalOrder::Before)
				relate(draft.document->handle + " is an earlier draft of " + signedOne.document->handle + ".", draft, signedOne);
			else if (order == TemporalOrder::After)
				relate(draft.document->handle + " is a draft made after " + signedOne.document->handle
					+ (isSigned ? " was signed" : "") + " - a proposed change to it.", draft, signedOne);
		}
	}
	for (const Placed &entry : conflicting) {
		const std::string statement = entry.document->handle + "'s name and its text give it different dates, so its order "
			"is not known.";
		lines.push_back(statement);
		result.relations.push_back({statement, {entry.document->document_id}, entry.dating.evidence});
	}

	result.text = "Worked out on the lawyer's computer from each document's own date. The dates themselves, and the time "
		"between them, were not sent.";
	for (const std::string &line : lines) result.text += "\n- " + line;
	return result;
}

} // namespace singularity::context::temporal_context
