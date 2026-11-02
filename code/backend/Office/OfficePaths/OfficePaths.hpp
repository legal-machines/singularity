#pragma once

#include <filesystem>
#include <optional>
#include <string>

// The rules for files a pane names (D-105), apart from any Matter: where a
// file Office names is on this computer, whether it is inside a folder, the
// name a folder can take, and the Base64 a pane hands bytes in.
namespace singularity::office {

// A file:// address as a path on this computer; anything else unchanged.
std::string path_of_location(const std::string &location);

// The file's place inside `folder` ("Drafts/Harborline NDA.docx"), through any
// link or junction either is reached by, or nothing when it is not inside.
// Names compare without case where the file systems Office runs on do.
std::optional<std::string> relative_inside(const std::filesystem::path &folder, const std::filesystem::path &file);

// Whether two places in a folder name the same file.
bool same_place(const std::string &one, const std::string &other);

// A file name a folder can take on every system: no separators, no reserved
// characters, not empty, not longer than 180 characters.
std::string safe_file_name(const std::string &name);

// `name` in `folder`, or "name (2).ext" and on, when it is taken.
std::filesystem::path free_path_in(const std::filesystem::path &folder, const std::string &name);

std::string encode_base64(const std::string &bytes);
std::optional<std::string> decode_base64(const std::string &text);

} // namespace singularity::office
