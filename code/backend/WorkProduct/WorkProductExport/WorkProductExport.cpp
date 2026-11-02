#include "WorkProduct/WorkProductExport/WorkProductExport.hpp"

#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"

#include <algorithm>
#include <stdexcept>

namespace singularity::work_product {

PreparedExport prepare_export(const std::string &matter_id, const std::string &source_id,
                              matters::ExportSourceType source_type,
                              const std::string &format) {
  if (format != "markdown" && format != "text")
    throw std::runtime_error("Choose a valid export format.");

  std::string content;
  std::string title;
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    const auto workspace_path = node::require_active_workspace_path();
    auto list = matters::load_matters(workspace_path);
    const auto found = std::find_if(list.begin(), list.end(),
                                    [&](const matters::Matter &matter) {
                                      return matter.id == matter_id;
                                    });
    if (found == list.end()) throw std::runtime_error("This matter no longer exists.");
    content = matters::resolve_export_content(*found, source_type, source_id);
    // The name the work already has. A save panel that proposes "Work Product"
    // for every export makes the lawyer name the file themselves every time,
    // and makes two exports of two different things collide.
    if (source_type == matters::ExportSourceType::WorkProduct) {
      const auto work = std::find_if(found->work_products.begin(), found->work_products.end(),
                                     [&](const matters::WorkProduct &candidate) {
                                       return candidate.id == source_id;
                                     });
      if (work != found->work_products.end()) title = work->title;
    } else if (source_type == matters::ExportSourceType::Note) {
      const auto note = std::find_if(found->notes.begin(), found->notes.end(),
                                     [&](const matters::MatterNote &candidate) {
                                       return candidate.id == source_id;
                                     });
      if (note != found->notes.end() && note->title) title = *note->title;
    }
  }

  const std::string extension = format == "markdown" ? "md" : "txt";
  std::string defaultName = "Work Product";
  if (source_type == matters::ExportSourceType::Task) defaultName = "Singularity result";
  else if (source_type == matters::ExportSourceType::Note) defaultName = "Matter note";
  // A file name, not a title: the characters a file system refuses are removed
  // and nothing else is changed. A title that is only such characters falls
  // back to the constant above rather than producing a nameless file.
  {
    std::string cleaned;
    cleaned.reserve(title.size());
    for (const char character : title) {
      const auto byte = static_cast<unsigned char>(character);
      if (byte < 0x20 || character == '/' || character == '\\' || character == ':' || character == '*'
          || character == '?' || character == '"' || character == '<' || character == '>' || character == '|') {
        continue;
      }
      cleaned.push_back(character);
    }
    while (!cleaned.empty() && (cleaned.back() == ' ' || cleaned.back() == '.')) cleaned.pop_back();
    while (!cleaned.empty() && cleaned.front() == ' ') cleaned.erase(cleaned.begin());
    if (cleaned.size() > 120) cleaned.resize(120);
    if (!cleaned.empty()) defaultName = cleaned;
  }
  return PreparedExport{
      std::move(content), defaultName + "." + extension,
      format == "markdown" ? "Markdown" : "Plain text", extension};
}

void write_export(const std::filesystem::path &path, const PreparedExport &prepared) {
  matters::write_export_file(path, prepared.content);
}

} // namespace singularity::work_product
