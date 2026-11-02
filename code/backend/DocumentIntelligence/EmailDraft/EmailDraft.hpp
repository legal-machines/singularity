#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace singularity::document_intelligence {

// A file attached to an email draft: where it is, and the name it goes by.
struct EmailAttachment {
	std::filesystem::path path;
	std::string name;
};

struct EmailDraft {
	std::string to;
	std::string cc;
	std::string subject;
	// Plain text, as the lawyer will read and edit it.
	std::string body;
	std::vector<EmailAttachment> attachments;
};

struct EmailDraftOutcome {
	bool isWritten = false;
	std::string problem;
	std::filesystem::path path;
	std::string sha256;
	// The names of the files attached, as the draft holds them.
	std::vector<std::string> attached;
};

// An email draft as a file the lawyer opens in their mail program (D-115): a
// standard Internet message (RFC 5322, MIME), marked unsent (X-Unsent: 1) so
// Outlook opens it as a new message to edit and send - Singularity never sends
// it. Each attachment is read whole into it. The file is then read back -
// recipients, subject, text and every attachment's bytes compared with what
// was meant - and kept only if all of it reads as meant. `target` must not
// exist; nothing is left at `target` when the draft could not be proven.
EmailDraftOutcome writeEmailDraft(const std::filesystem::path& target, const EmailDraft& draft);

// An email file as it reads: its recipients, subject and plain text, whether
// it is marked unsent, its identities - the Message-ID every mailbox gives it
// alike, and the ones it answers - and each attachment's name and bytes.
struct ReadEmail {
	bool isRead = false;
	bool isUnsent = false;
	std::string messageId;
	std::string inReplyTo;
	std::string references;
	std::string from;
	std::string to;
	std::string cc;
	std::string date;
	std::string subject;
	std::string body;
	std::vector<std::pair<std::string, std::string>> attachments;
};
ReadEmail readEmailFile(const std::filesystem::path& path);
ReadEmail readEmailText(const std::string& message);

} // namespace singularity::document_intelligence
