#include "Document/DocumentActions/DocumentActions.hpp"

#include "Document/DocumentExtractor/DocumentExtractor.hpp"
#include "Document/DocumentFileActions/DocumentFileActions.hpp"
#include "Document/DocumentRegistry/DocumentRegistry.hpp"
#include "Document/DocumentStructuralReader/DocumentStructuralReader.hpp"
#include "Document/DocumentVersionStore/DocumentVersionStore.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/OpenExternal/OpenExternal.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace singularity::document {
namespace {

auto find(std::vector<matters::Matter> &all, const std::string &id) {
  return std::find_if(all.begin(), all.end(), [&](const matters::Matter &entry) { return entry.id == id; });
}

// The one real current path for a document, regardless of source_mode: the
// internal copy directory for Managed (Phase 03), or the real Project file
// for Referenced (Phase 08) - reveal()/open() must never guess which. The
// recorded name or relative path is persisted Project data (shared `.lm`
// state can be edited by another writer), so it is never trusted to leave
// its directory: an absolute path, a ".." component or a symlink that
// resolves outside the Project folder is refused.
std::filesystem::path resolve_document_path(const std::filesystem::path& workspace,
    const matters::Matter& matter, const matters::MatterDocument& document) {
  if (document.source_mode == matters::DocumentSourceMode::Managed) {
    const auto managed = matters::managed_document_path(workspace, matter.id, document.stored_name);
    if (!managed || !documents::registry::is_relative_path_safe(managed->parent_path(), document.stored_name)) {
      throw std::runtime_error("This document's recorded location is not inside Singularity's document store, so it "
          "was not opened.");
    }
    return *managed;
  }
  if (!matter.directory_path || matter.directory_path->empty()) {
    throw std::runtime_error("Choose this Matter's project folder first.");
  }
  if (!documents::registry::is_relative_path_safe(*matter.directory_path, document.relative_path)) {
    throw std::runtime_error("This document's recorded location is outside its Project folder, so it was not "
        "opened.");
  }
  return std::filesystem::path(*matter.directory_path) / document.relative_path;
}

} // namespace

void require_matter(const std::string &matter_id) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  auto all = matters::load_matters(node::require_active_workspace_path());
  if (find(all, matter_id) == all.end()) throw std::runtime_error("This matter no longer exists.");
}

matters::Matter import_files(const std::string &matter_id, const std::vector<std::string> &paths) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace = node::require_active_workspace_path();
  auto found = matters::load_matter(workspace, matter_id);
  if (!found) throw std::runtime_error("This matter no longer exists.");
  if (paths.empty()) return *found;
  // A Matter with a folder keeps its documents there, beside its `.lm`: a file
  // added is copied into the folder, where the lawyer finds it, its AI
  // colleagues read it and the firm's computers carry it, sealed (D-123). Only
  // a Matter with no folder keeps a copy in Singularity's own store.
  if (found->directory_path.has_value() == true && found->directory_path->empty() == false) {
    matters::Matter added = *found;
    // A name already in the folder keeps both, as a dropped file does: a
    // lawyer adding "Brief.pdf" twice, or a picture already sent in another
    // personal conversation, is never refused for its name.
    for (const auto &path : paths) added = add_external_file(matter_id, "", path, false, true);
    return added;
  }
  const auto destination = matters::documents_directory(workspace, matter_id);
  std::filesystem::create_directories(destination);
  for (const auto &path : paths) {
    const std::filesystem::path source(path);
    matters::MatterDocument added;
    added.id = matters::generate_document_id();
    added.matter_id = matter_id;
    added.name = source.filename().string();
    added.stored_name = added.id + "_" + added.name;
    added.file_type = documents::detect_document_format(added.name);
    added.added_at = workspaces::current_timestamp();
    std::error_code size_error;
    const auto size = std::filesystem::file_size(source, size_error);
    added.size_bytes = size_error ? 0 : static_cast<long long>(size);
    std::error_code copy_error;
    std::filesystem::copy_file(source, destination / added.stored_name,
                               std::filesystem::copy_options::overwrite_existing, copy_error);
    if (copy_error) throw std::runtime_error("Could not import \"" + added.name + "\": " + copy_error.message());
    (void)matters::refresh_document_derivatives(workspace, matter_id, added);
    found->documents.push_back(std::move(added));
  }
  found->updated_at = workspaces::current_timestamp();
  matters::save_matter(workspace, *found);
  return *found;
}

matters::Matter remove(const std::string &matter_id, const std::string &document_id) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace = node::require_active_workspace_path();
  auto found = matters::load_matter(workspace, matter_id);
  if (!found) throw std::runtime_error("This matter no longer exists.");
  const auto document = std::find_if(found->documents.begin(), found->documents.end(),
      [&](const matters::MatterDocument &entry) { return entry.id == document_id; });
  if (document != found->documents.end()) {
    // Only Singularity's own copy is deleted, and only when the recorded name
    // is one file in its store: a name another writer set to "../../x" or an
    // absolute path deletes nothing.
    if (const auto managed = matters::managed_document_path(workspace, matter_id, document->stored_name)) {
      std::error_code remove_error;
      std::filesystem::remove(*managed, remove_error);
    }
    found->documents.erase(document);
    found->updated_at = workspaces::current_timestamp();
    matters::save_matter(workspace, *found);
  }
  return *found;
}

std::filesystem::path file_of(const std::string &matter_id, const std::string &document_id) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace = node::require_active_workspace_path();
  auto found = matters::load_matter(workspace, matter_id);
  if (!found) throw std::runtime_error("This matter no longer exists.");
  const auto document = std::find_if(found->documents.begin(), found->documents.end(),
      [&](const matters::MatterDocument &entry) { return entry.id == document_id; });
  if (document == found->documents.end()) throw std::runtime_error("This document no longer exists.");
  return resolve_document_path(workspace, *found, *document);
}

bool reveal(const std::string &matter_id, const std::string &document_id) {
  std::filesystem::path path;
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    const auto workspace = node::require_active_workspace_path();
    auto found = matters::load_matter(workspace, matter_id);
    if (!found) throw std::runtime_error("This matter no longer exists.");
    const auto document = std::find_if(found->documents.begin(), found->documents.end(),
        [&](const matters::MatterDocument &entry) { return entry.id == document_id; });
    if (document == found->documents.end()) throw std::runtime_error("This document no longer exists.");
    path = resolve_document_path(workspace, *found, *document);
  }
  return platform::reveal_in_file_manager(path.string());
}

bool open(const std::string& matter_id, const std::string& document_id) {
  std::filesystem::path path;
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    const auto workspace = node::require_active_workspace_path();
    auto found = matters::load_matter(workspace, matter_id);
    if (!found) throw std::runtime_error("This matter no longer exists.");
    const auto document = std::find_if(found->documents.begin(), found->documents.end(),
        [&](const matters::MatterDocument& entry) { return entry.id == document_id; });
    if (document == found->documents.end()) throw std::runtime_error("This document no longer exists.");
    // Opening hands the file to the system's default application, which
    // chooses by file type: the file on disk decides below whether it is a
    // document or a picture that may be handed over - a photo in a personal
    // conversation (D-212) is no document format of the record's, and opened
    // all the same; anything else can still be revealed in the file manager.
    path = resolve_document_path(workspace, *found, *document);
  }
  // What the record says a file is decides nothing: the file on disk must be a
  // document itself (a shared folder's record once named a ".command" a PDF).
  if (!platform::may_open_as_document(path.u8string()))
    throw std::runtime_error(std::string("Singularity opens only documents and pictures. Reveal this file in ")
        + platform::file_manager_name() + " to open it.");
  return platform::open_in_default_application(path.u8string());
}


DocumentDisplayReading read_for_display(const std::string& matter_id, const std::string& document_id) {
  std::filesystem::path path;
  std::filesystem::path versionsRoot;
  DocumentDisplayReading reading;
  documents::DocumentFormat format = documents::DocumentFormat::unknown;
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    const auto workspace = node::require_active_workspace_path();
    auto found = matters::load_matter(workspace, matter_id);
    if (!found) throw std::runtime_error("This matter no longer exists.");
    const auto document = std::find_if(found->documents.begin(), found->documents.end(),
        [&](const matters::MatterDocument &entry) { return entry.id == document_id; });
    if (document == found->documents.end()) throw std::runtime_error("This document no longer exists.");
    path = resolve_document_path(workspace, *found, *document);
    versionsRoot = documents::document_versions_root(found->directory_path, workspace, matter_id);
    reading.document_id = document->id;
    reading.document_name = document->name;
    reading.version = document->version;
    reading.fingerprint = document->fingerprint;
    format = document->file_type;
  }
  // Read outside the persistence lock: laying out a long document must never
  // hold up the rest of the Project.
  documents::DocumentExtractionRequest request;
  request.path = path;
  request.format_hint = format;
  reading.model = documents::read_document_render_model(request);
  // A CSV or TSV is one sheet named after its file: the name the Project knows
  // it by, not the name Singularity's own copy of it happens to be stored under.
  if (reading.model.workbook && reading.model.workbook->sheets.size() == 1
      && (reading.model.format == documents::DocumentFormat::csv
          || reading.model.format == documents::DocumentFormat::tsv)) {
    const std::size_t extension = reading.document_name.rfind('.');
    reading.model.workbook->sheets.front().name = extension == std::string::npos || extension == 0
        ? reading.document_name : reading.document_name.substr(0, extension);
  }
  // What the last change did, against the version Singularity kept before it wrote.
  // A document Singularity never changed has nothing kept, and every mark stays
  // Unknown - which is what the reader must show rather than "unchanged". A
  // workbook is compared cell by cell, on the text each cell shows: a cell
  // that shows something else carries what it showed, and what a sheet no
  // longer has is listed as removed (mark_document_changes).
  const auto kept = documents::latest_kept_document_version(versionsRoot, document_id, reading.fingerprint);
  if (kept) {
    documents::DocumentExtractionRequest previousRequest;
    previousRequest.path = kept->path;
    previousRequest.format_hint = format;
    documents::mark_document_changes(reading.model,
        documents::read_document_render_model(previousRequest), kept->version);
  }
  return reading;
}

} // namespace singularity::document
