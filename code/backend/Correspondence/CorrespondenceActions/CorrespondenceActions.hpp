#pragma once

#include "Correspondence/LetterRouting/LetterRouting.hpp"
#include "Correspondence/Types/Correspondence.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

// Filing letters to their Matters, the smallest correct version: a member
// files a letter from their mail client, on the evidence Singularity shows
// them (Correspondence/LetterRouting). Nothing is filed by guess, and
// nothing is sent. A filed letter is the Matter's, seen by its members on
// every computer that opens it.
namespace singularity::correspondence {

class CorrespondenceRefused : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

// Where a message belongs among the Matters open on this computer. It reads
// them and writes nothing: a suggestion is the member's own, never shared.
Suggestion suggest(const MessageFacts &facts);

// Files the letter to the Matter as the person at this computer, with the
// evidence they saw, from `filed_from` ("outlook"). The same letter filed
// again there is the one already filed; a letter filed in another Matter is
// refused (it is moved instead). A letter not yet sent or received (no
// Message-ID) is refused. Filed `by_rule`, it is filed by a rule of the
// Matter's intake, which must answer to it here and now.
Letter file_letter(const std::string &matter_id, const MessageFacts &facts, const std::vector<std::string> &evidence,
	const std::string &filed_from, bool by_rule = false);

// Moves a filed letter to another Matter open here, saying who moved it and
// where it was first filed.
Letter move_letter(const std::string &matter_id, const std::string &letter_id, const std::string &to_matter_id);

// Takes a letter out of its Matter. The documents saved from it stay.
void unfile_letter(const std::string &matter_id, const std::string &letter_id);

// Files each email a member put in the Matter's folder that was sent or
// received (an .eml with a Message-ID, never a draft marked unsent) as one of
// its letters, from "folder": its id made from its Message-ID, so the same
// email filed on two computers is one letter; one filed here already, or in
// another Matter here, is left as it is. Its attachments are listed, never
// saved: they are the Matter's documents only when a person saves them. The
// letters it filed.
std::vector<Letter> file_folder_letters(const std::string &matter_id);

// The Matter's letters, the latest first: the emails in its folder filed first.
std::vector<Letter> letters_of(const std::string &matter_id);

// A reply to a letter filed to the Matter, as the member's mail app opens it
// to edit and send: to whoever wrote it - or, for one the firm sent, to whom
// it went first - with "Re: " before its subject once, and the member's
// words. Its address is empty where the letter names none.
struct ReplyDraft {
	std::string to;
	std::string subject;
	std::string body;
};
ReplyDraft reply_draft(const Letter &letter, const std::string &text);

// A reply that hands over files: the reply_draft above as an unsent email
// (X-Unsent: 1) with the Matter's documents attached, written to
// Singularity's own folder on this computer - never the Matter's, so nothing
// of a draft reaches the firm's other computers - for the member's mail app
// to open, edit and send. Singularity never sends it. Drafts a week old are
// cleared from that folder as a new one is written. Refused in words when the
// letter names no address or a document cannot be attached.
struct ReplyFile {
	std::filesystem::path path;
	std::vector<std::string> attached;
	// The documents' own files, as the draft carries them.
	std::vector<std::filesystem::path> files;
};
ReplyFile write_reply_with_files(const std::string &matter_id, const Letter &letter, const std::string &text,
	const std::vector<std::string> &document_ids);

// Saves one of a filed letter's attachments to the Matter's folder. Bytes the
// Matter already holds are not written twice: the attachment is linked to
// that file.
LetterAttachment save_attachment(const std::string &matter_id, const std::string &letter_id,
	const std::string &provider_id, const std::string &bytes);

} // namespace singularity::correspondence
