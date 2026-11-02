#pragma once

#include <optional>
#include <string>
#include <string_view>

// THE DATA ROOM'S JOURNAL
//
// What happened to a Project's documents, as Singularity noticed it: a document
// appeared, its content changed, it was renamed or moved, it went away, it
// came back. One record per event, written by the document registry at the
// moment it records the transition itself (Document/DocumentRegistry) - an
// observation of the file system, never an inference - and kept in the
// Project's own store (.lm), so it travels with the Project like everything
// else in it.
//
// It is what "what is new in the data room?" is answered from: every request
// is given the recent journal as records of its own (see
// MatterRequestExecution.cpp), and nothing about such a question is decided in
// code.
namespace singularity::documents {

enum class DocumentEventKind { Added, Changed, Moved, Removed, Returned };

struct DocumentEvent {
	std::string id;
	std::string document_id;
	DocumentEventKind kind = DocumentEventKind::Added;
	// When Singularity noticed it, and - when the file says - when the file itself
	// was last modified.
	std::string at;
	std::string file_modified_at;
	// The document's name and place in the Project at that moment.
	std::string name;
	std::string relative_path;
	// Moved: where it was and what it was called.
	std::string previous_name;
	std::string previous_relative_path;
	// Changed: which version it became, from which; the content before and after.
	int version = 0;
	int previous_version = 0;
	std::string fingerprint;
	std::string previous_fingerprint;
	long long size_bytes = 0;
	long long previous_size_bytes = 0;
	// Who made the change: empty when it was found on disk, "singularity" when an
	// Singularity operation wrote it - then with that operation's id.
	std::string actor;
	std::string operation_id;
	// An Singularity write: the person whose Singularity wrote it
	// (Platform/ActorIdentity actor id) and on which computer. In a Matter
	// shared between computers this is what tells a colleague's change, made
	// through Singularity, from one nobody can name. Empty for a change found
	// on disk, and for one recorded before they were kept.
	std::string actor_id;
	std::string device_id;
};

[[nodiscard]] inline const char *to_string(DocumentEventKind kind) noexcept {
	switch (kind) {
		case DocumentEventKind::Added: return "added";
		case DocumentEventKind::Changed: return "changed";
		case DocumentEventKind::Moved: return "moved";
		case DocumentEventKind::Removed: return "removed";
		case DocumentEventKind::Returned: return "returned";
	}
	return "added";
}

[[nodiscard]] inline std::optional<DocumentEventKind> document_event_kind_from_string(std::string_view value) noexcept {
	if (value == "added") return DocumentEventKind::Added;
	if (value == "changed") return DocumentEventKind::Changed;
	if (value == "moved") return DocumentEventKind::Moved;
	if (value == "removed") return DocumentEventKind::Removed;
	if (value == "returned") return DocumentEventKind::Returned;
	return std::nullopt;
}

// A journal is a record of recent history, not an archive: the oldest events
// fall off once it holds this many.
inline constexpr std::size_t kMaximumDocumentEvents = 2000;

} // namespace singularity::documents
