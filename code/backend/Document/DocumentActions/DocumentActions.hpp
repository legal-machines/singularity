#pragma once

#include "Document/Types/DocumentRenderTypes.hpp"
#include "Matter/Types/Matter.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace singularity::document {

void require_matter(const std::string &matter_id);
matters::Matter import_files(const std::string &matter_id, const std::vector<std::string> &paths);
matters::Matter remove(const std::string &matter_id, const std::string &document_id);

// Both resolve the document's real current file regardless of
// source_mode (Managed copy vs. Phase 08 Referenced Project file) - see
// DocumentActions.cpp.
bool reveal(const std::string &matter_id, const std::string &document_id);

// The document's real current file on this computer, as reveal() and open()
// find it; refused in words when its recorded place is not inside the Matter.
std::filesystem::path file_of(const std::string &matter_id, const std::string &document_id);

// Phase 09's "Open document": launches the file with the OS default
// application (Platform/OpenExternal/OpenExternal.hpp). Word stays the place
// a lawyer EDITS a document by hand; read_for_display() below is where they
// LOOK at it without leaving their work.
bool open(const std::string& matter_id, const std::string& document_id);

// This document as its author laid it out, read fresh from the real current
// file every time it is asked for. `version` and `fingerprint` are the
// Project's own identity for the bytes that were read, so a reader can tell
// whether what it is showing is still what a change was approved against.
struct DocumentDisplayReading {
  std::string document_id;
  std::string document_name;
  int version = 0;
  std::string fingerprint;
  documents::DocumentRenderModel model;
};

// Never mutates anything and never starts a reconcile: opening a document to
// look at it must be as safe and as quiet as opening a folder.
DocumentDisplayReading read_for_display(const std::string& matter_id, const std::string& document_id);

} // namespace singularity::document
