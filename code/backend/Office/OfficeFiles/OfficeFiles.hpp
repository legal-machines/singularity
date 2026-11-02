#pragma once

#include <optional>
#include <string>

// The file a pane is open in, and its Matter (D-105). A document or workbook
// Word or Excel has open, or a message's attachment in Outlook, is a file; a
// Matter is the folder its documents live in. These say which Matter a file
// belongs to, put a file into a Matter's folder, and hand a pane a Matter
// document's bytes - nothing else. The rules for the paths and names
// themselves are Office/OfficePaths.
namespace singularity::office {

struct FileInMatter {
	std::string matter_id;
	std::string matter_name;
	// The file's place in the Matter's folder ("Drafts/Harborline NDA.docx").
	std::string relative_path;
	// The Matter's document it is, once Singularity has read it; empty before.
	std::string document_id;
};

struct DescribedFile {
	// "in_matter": in a Matter's folder; "outside": on this computer, in no
	// Matter; "online": a file Office opened from the web, which Singularity
	// cannot place; "unsaved": a new file, not saved yet; "starter": the file
	// Singularity opened Word, Excel or PowerPoint with (Office/OfficeApps).
	std::string kind;
	std::string name;
	std::optional<FileInMatter> place;
};

// Where the file Office names is: a path, a file:// address or a web address.
DescribedFile describe_file(const std::string &location);

struct Filed {
	std::string matter_id;
	std::string relative_path;
	std::string name;
};

// Puts a file into the Matter's folder under a free name; Singularity reads it
// in as it reads any document added there. Throws std::runtime_error when the
// Matter has no folder here or the file cannot be written.
Filed file_into_matter(const std::string &matter_id, const std::string &name, const std::string &bytes);

struct DocumentContent {
	std::string name;
	std::string bytes;
};

// A Matter document's bytes, for a pane that attaches it to a message. Throws
// std::runtime_error when the document is not here or too large to attach.
DocumentContent document_content(const std::string &matter_id, const std::string &document_id);

// Opens a Matter document in the application the system opens its kind with
// - a colleague's draft in Word, beside the one the person is in. A file that
// is not a document format is never opened (Platform/OpenExternal). Throws
// std::runtime_error when it is not here or cannot be opened.
void open_document(const std::string &matter_id, const std::string &document_id);

} // namespace singularity::office
