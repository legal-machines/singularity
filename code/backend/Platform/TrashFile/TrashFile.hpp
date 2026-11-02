#pragma once

#include <optional>
#include <string>

namespace singularity::platform {

// Trash is a real OS capability, not merely "worked" / "didn't work" - a
// caller must never conflate "this platform doesn't have Trash" with "Trash
// was attempted and failed" (Phase 09: an ordinary "Remove" must never fall
// back to a permanent delete in either case - see
// Document/DocumentFileActions/DocumentFileActions.hpp's remove_document()).
enum class TrashResult { Succeeded, Unsupported, Failed };

// Moves a local file to the OS Trash/Recycle Bin: NSFileManager on macOS
// (TrashFileMacOS.mm), the shell's Recycle Bin on Windows
// (TrashFileWindows.cpp), the freedesktop.org Trash specification on Linux
// (TrashFileLinux.cpp).
// - Succeeded: the file is now in the OS Trash.
// - Unsupported: this platform has no Trash capability - the file was never
//   touched. Every platform built today has one; callers still refuse on it.
// - Failed: Trash is supported here, but this specific attempt failed (the
//   OS call itself failed) - the file was never touched.
TrashResult move_to_trash(const std::string& path);

// The OS Trash by the name people know it, for a sentence such as "moved to
// the Trash": Trash on macOS and Linux, Recycle Bin on Windows.
const char* trash_name();

// Test-only seam: forces move_to_trash()'s result for the remainder of this
// process, without depending on real, hard-to-force-deterministically OS/
// filesystem failure conditions - mirrors Platform/FileSystemWatcher's own
// testing override. std::nullopt restores real behavior. Never read by
// normal production code paths.
void set_trash_result_override_for_testing(std::optional<TrashResult> value);

} // namespace singularity::platform
