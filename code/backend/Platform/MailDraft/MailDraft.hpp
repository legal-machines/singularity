#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace singularity::platform {

// A new message in the member's own mail app, with files attached, for them
// to read, change and send there; Singularity never sends it. `eml` is the
// same draft written as an unsent email (X-Unsent: 1).
//
// macOS: when Mail is the mail app (the mailto: handler), the system's
// compose-email service opens Mail's new message with the recipient, subject,
// words and files - Mail shows an .eml as a message received, never as a
// draft. With another mail app, such as Outlook, the .eml opens in that app.
// Windows and Linux: the .eml opens with its own application, which Outlook
// opens as a new message.
//
// Linux, later: Thunderbird, the usual mailto: handler there, also shows an
// .eml as a message received; `thunderbird -compose "to=…,subject=…,body=…,
// attachment='file:///…'"` would open it as a draft.
//
// False when no mail app could be asked to open it: neither Mail's compose
// service, nor the mail app, nor the .eml's own application took it. The
// .eml stays where it was written either way.
bool open_mail_draft(const std::string &to, const std::string &subject, const std::string &body,
                     const std::vector<std::filesystem::path> &files, const std::filesystem::path &eml);

} // namespace singularity::platform
