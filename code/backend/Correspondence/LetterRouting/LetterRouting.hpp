#pragma once

#include "Correspondence/Types/Correspondence.hpp"
#include "Matter/Types/Matter.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Where a letter belongs (D-106), told from evidence, strongest first:
//   - the letter itself is filed already (its Internet Message-ID);
//   - a letter of the same conversation is filed (the provider's own
//     conversation id, never an equal subject);
//   - someone in it is in a letter filed there (their exact address, never a
//     shared domain alone);
//   - its subject or an attachment names the Matter or one of its documents.
// One Matter on that evidence is a suggestion; more than one is ambiguous,
// unless only one holds the conversation; none is unfiled. No confidence
// figure is made up. Pure: it reads the Matters it is given and nothing else.
namespace singularity::correspondence {

// What a mail client knows of a message: enough to file it and to tell where
// it belongs.
struct MessageFacts {
	std::string internet_message_id;
	std::string conversation_id;
	// The address of the mailbox it is read in.
	std::string mailbox;
	std::string subject;
	Party from;
	std::vector<Party> to;
	std::vector<Party> cc;
	std::string sent_at;
	std::string text;
	std::vector<LetterAttachment> attachments;
};

// One Matter the evidence points to, and why, in sentences.
struct MatterEvidence {
	std::string matter_id;
	std::string matter_name;
	// "filed", "rule", "conversation", "participant" or "reference": its strongest.
	std::string strongest;
	std::vector<std::string> evidence;
	// The letter, where it is filed there.
	std::string letter_id;
};

struct Suggestion {
	// "filed", "rule" (one Matter's intake files it, D-228), "suggested",
	// "ambiguous" or "unfiled".
	std::string outcome;
	// The Matters in the order of their evidence, strongest first.
	std::vector<MatterEvidence> matters;
};

Suggestion suggest_among(const std::vector<matters::Matter> &matters, const MessageFacts &facts);

// The rule of the Matter's intake a letter answers to - the account it reached
// and the mailbox it came from, each exactly - or null (D-228).
const IntakeRule *intake_rule_for(const matters::Matter &matter, const MessageFacts &facts);

// Text on one line, its runs of spaces as one, at most `most` bytes, cut
// where a character begins.
std::string one_line(const std::string &value, std::size_t most);

// At most `most` bytes, cut where a character begins.
std::string cut_text(const std::string &text, std::size_t most);

// An address as it is compared: on one line, in lower case.
std::string address_of(const Party &party);

// "2026-10-24" of a moment written in ISO 8601.
std::string day_of(const std::string &moment);

// The letter filed in the Matter with this Internet Message-ID, or null.
const Letter *letter_with_message_id(const matters::Matter &matter, const std::string &internet_message_id);

// ---- An email a member puts in the Matter's folder (D-180) ---------------------

// An email file as it was read (an .eml): its headers as they stand, words
// decoded, and its plain text and attachments.
struct EmailFile {
	bool is_unsent = false;
	std::string message_id;
	std::string in_reply_to;
	std::string references;
	std::string from;
	std::string to;
	std::string cc;
	std::string date;
	std::string subject;
	std::string text;
	// Each attachment's name and size in bytes.
	std::vector<std::pair<std::string, long long>> attachments;
};

// The people of an address line ("Elena Marsh <elena@harborview.example>,
// \"Bell, Marcus\" <marcus@whitfield.example>"), in its order; a group's name
// and an entry with no address are not people.
std::vector<Party> parties_of(const std::string &header);

// A message's Date header ("Tue, 27 Oct 2026 09:14:00 -0400") as the moment
// it names, in ISO 8601 in UTC ("2026-10-27T13:14:00Z"); "" when it names
// none.
std::string moment_of(const std::string &date);

// The conversation a message belongs to, by the message it began with: the
// first Message-ID its References name, else the one it answers, else its
// own.
std::string conversation_of(const EmailFile &email);

// What an email file of the folder is as a letter: nothing when it was never
// sent or received - a draft marked unsent, or one with no Message-ID - and
// otherwise its facts, as a mail client would give them.
std::optional<MessageFacts> facts_of_email(const EmailFile &email);

} // namespace singularity::correspondence
