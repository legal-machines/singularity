#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <vector>

namespace singularity::bridge::document_bridge {

void validate_import(const nlohmann::json &params);
nlohmann::json import_files(const nlohmann::json &params,
                            const std::vector<std::string> &selected_paths);
nlohmann::json remove(const nlohmann::json &params);
nlohmann::json reveal(const nlohmann::json &params);

// Phase 09's "Open document" - launches the real current file with the OS
// default application (Document/DocumentActions/DocumentActions.hpp::open()).
nlohmann::json open(const nlohmann::json& params);

// Phase 08 (Documents - filesystem, list and states): reconciles and
// returns this Matter's full document list, real Project-folder files
// included - see Matter/MatterActions/MatterActions.hpp::list_documents().
nlohmann::json list_documents(const nlohmann::json& params);

// Phase 09 real filesystem actions for a Referenced document
// (Document/DocumentFileActions/DocumentFileActions.hpp) - each performs the
// real operation on the Project folder, then returns the freshly
// reconciled Matter.
nlohmann::json rename_document(const nlohmann::json& params);
nlohmann::json move_document(const nlohmann::json& params);
// Distinct from remove() above: that removes a Managed (Phase 03) copy;
// this deletes a real Referenced Project file via the OS Trash only - it
// throws (never returns) when Trash is unsupported or fails, so callers
// never see a "permanent delete" result shape - see
// Document/DocumentFileActions/DocumentFileActions.hpp::remove_document().
nlohmann::json delete_document(const nlohmann::json& params);
nlohmann::json create_folder(const nlohmann::json& params);
nlohmann::json add_external_file(const nlohmann::json& params);
nlohmann::json reprocess_document(const nlohmann::json& params);

// Phase 09's local source-resolution contract
// (Document/DocumentSourceResolver/DocumentSourceResolver.hpp) - resolves a
// real citation (documentId + chunkId) or, with chunkId omitted/empty,
// Documents' own "Preview source" entry point (the Document's first real
// chunk) back to a truthful local preview. Never fabricates a page/heading/
// preview and never silently binds a historical chunkId to changed bytes.
nlohmann::json resolve_document_source(const nlohmann::json& params);

// The document as its author laid it out - headings, emphasis, lists, tables
// and cell shading - read fresh from the real current file so a lawyer can
// see what Singularity changed without leaving their work for Word. Read-only:
// it never reconciles, mutates or caches. A format Singularity cannot lay out
// still returns its text, and says why in "limitation" rather than
// pretending to a layout it never read.
nlohmann::json read_document_view(const nlohmann::json& params);

// The versions Singularity kept of a Project document before it changed it, and
// putting one back through the same verified, staged commit every Singularity
// write takes (Document/ProfessionalArtifactAction: list_document_versions,
// restore_document_version). A restore refuses, in plain words, a document
// that changed since its versions were shown, is open in its program or is
// locked - nothing is written then.
nlohmann::json list_versions(const nlohmann::json& params);
nlohmann::json restore_version(const nlohmann::json& params);

} // namespace singularity::bridge::document_bridge
