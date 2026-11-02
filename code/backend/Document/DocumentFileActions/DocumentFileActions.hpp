#pragma once

#include "Matter/Types/Matter.hpp"

#include <string>

namespace singularity::document {

// Real filesystem actions for a Referenced (real Project-folder) document -
// Phase 09 (documents-actions-source-peek.md). Every function here performs
// the actual std::filesystem operation on the real Project folder FIRST,
// then hands off to the exact same reconciliation pipeline Phase 08's
// watcher already uses (Matter/MatterActions/MatterActions.hpp's
// detect_affected_paths()/process_project_documents()) to derive canonical
// identity/version/state - this component never writes MatterDocument
// fields itself (request_reprocessing() below is the one narrow exception,
// and it lives in DocumentRegistry, not here). See DocumentActions.hpp for
// the separate, older action surface that still operates on Managed
// (copied-in) documents only.
//
// `expected_version` on every function that targets one specific document is
// a staleness guard (PHASE 09 spec Sec.14 "External filesystem changes while
// actions are open"). This is checked against freshly re-derived truth, not
// a possibly-stale in-memory/persisted snapshot: before mutating, every
// function first re-runs Phase 08's own targeted reconciliation
// (detect_affected_paths()) over this document's last-known real path -
// exactly what a live filesystem watcher would have produced by now if one
// happened to fire first - so a same-path external replacement, an external
// move-away, or an external delete that raced ahead of the (asynchronous)
// watcher is caught before any destructive action, never after. If the file
// changed, disappeared or no longer matches `expected_version`, the action
// refuses instead of acting on whatever now occupies the stale path.

// Renames a Referenced document in place (same folder, new file name).
matters::Matter rename_document(const std::string& matter_id, const std::string& document_id,
                                int expected_version, const std::string& new_name);

// Moves a Referenced document to a different folder already present inside
// the Project. `destination_folder_relative_path` is "" for the Project
// root.
matters::Matter move_document(const std::string& matter_id, const std::string& document_id,
                              int expected_version, const std::string& destination_folder_relative_path);

// Deletes a Referenced document's real file by moving it to the OS Trash.
// This ordinary "Remove" command succeeds ONLY when Trash genuinely
// receives the file (Platform/TrashFile/TrashFile.hpp's TrashResult::
// Succeeded) - it never silently falls back to a permanent delete, whether
// Trash is unsupported on this platform or the OS Trash call itself failed.
// A caller that genuinely needs permanent deletion where Trash is
// unsupported needs a separate, explicitly-consequential command; none
// exists yet. On success, DocumentRegistry's existing reconciliation is
// what turns the missing file into the historically-meaningful Unavailable
// state (never a silently vanished Document) - see Document/
// DocumentRegistry/DocumentRegistry.hpp.
matters::Matter remove_document(const std::string& matter_id, const std::string& document_id,
                                int expected_version);

// Creates a real folder inside the Project.
// `parent_folder_relative_path` is "" for the Project root.
matters::Matter create_folder(const std::string& matter_id, const std::string& parent_folder_relative_path,
                              const std::string& folder_name);

// Adds an external file into the Project folder: a real copy by default, or
// an explicit real move when `move_source` is true. Never uploads anywhere -
// this is a local filesystem operation only. Refuses rather than silently
// overwriting an existing file at the destination. An explicit Move never
// reports success while the original file still exists: if the original
// cannot be removed after a cross-device copy, the copy is rolled back (or,
// if rollback itself fails, an explicit partial-failure error is raised -
// never a silent success with two copies). With `keep_both`, a name already
// taken at the destination gives way to the next free one ("Brief 2.pdf"), as
// Finder's "Keep Both" and a file dropped into a Matter have it.
matters::Matter add_external_file(const std::string& matter_id, const std::string& destination_folder_relative_path,
                                  const std::string& source_absolute_path, bool move_source, bool keep_both = false);

} // namespace singularity::document
