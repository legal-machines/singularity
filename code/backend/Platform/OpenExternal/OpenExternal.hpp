#pragma once

#include <string>

namespace singularity::platform {

bool open_external_https(const std::string &url);

// A new message in the system's mail app (Outlook, where the firm uses it), to
// one address, with a subject and a body filled in, for the person to edit and
// send with their mail app's own Send: Singularity never sends it. A mailto:
// link (RFC 6068) carrying exactly those three: the address only when it is
// one plain address (local@domain, at most 254 characters, none of? & # %,; <
// > quotes, spaces, line breaks or control characters - it comes from mail,
// and could otherwise add a copy or an attachment), the subject and body as
// UTF-8 with every byte but the unreserved ones percent-encoded and line
// breaks as CR LF. A body that would make the link longer than mail apps take
// is never cut: the link leaves it out (`carries_body` false) and the caller
// hands the whole text over another way.
struct MailCompose {
  std::string link;  // empty: the address was refused
  bool carries_body = false;
};
MailCompose mail_compose_link(const std::string &to, const std::string &subject, const std::string &body);
bool is_plain_mail_address(const std::string &address);
// Opens the link: only a string that begins "mailto:", through the system's
// own opener with no shell. `opened` false when the address was refused or
// nothing opened.
struct MailComposed {
  bool opened = false;
  bool carries_body = false;
};
MailComposed open_mail_compose(const std::string &to, const std::string &subject, const std::string &body);

// Reveals a local file in the OS file manager (Finder/Explorer) - "show me
// this matter document". Best effort: returns false rather than throwing
// if the OS call fails, since this is a convenience action nothing else in
// the app depends on. On Linux this opens the file's containing folder
// (no cross-desktop-environment way to also select the file within it).
bool reveal_in_file_manager(const std::string &path);

// The file manager's name as people know it, for a sentence such as
// "Reveal this file in Finder": Finder, File Explorer, or "the file manager"
// on Linux, where it differs from desktop to desktop.
const char *file_manager_name();

// Opens a concrete local directory as a directory, rather than selecting an
// arbitrary child document. Used by Matter's first-class Folder action.
bool open_directory_in_file_manager(const std::string &path);

// Opens a local file with the OS default application for its type (Preview,
// Word, ...) - Phase 09's "Open document" action. Singularity does not ship its
// own document viewer/renderer; this is deliberately the whole viewer
// architecture. Best effort, like the functions above.
bool open_in_default_application(const std::string& path);

// Whether a file may be handed to the system's default application: a document
// format by the real file's own extension, its links resolved - never by what
// a record says it is. A file a record calls "Engagement letter.pdf" that is a
// ".command", an app or a script would run when opened; such a file is only
// ever revealed in the file manager.
bool may_open_as_document(const std::string& path);

// Whether a folder is a package the system would launch rather than show
// ("Tool.app", "Installer.pkg"): such a folder is revealed, never opened.
bool is_launchable_package(const std::string& path);

} // namespace singularity::platform
