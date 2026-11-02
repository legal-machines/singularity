#pragma once

#include "Platform/Types/ParentWindow.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace singularity::platform {

class NativeDialogs final {
public:
  NativeDialogs();
  ~NativeDialogs();

  NativeDialogs(const NativeDialogs &) = delete;
  NativeDialogs &operator=(const NativeDialogs &) = delete;

  // Where every dialog finds the window it opens over, asked as the dialog
  // opens, since a window has its native handle only once it is shown.
  // Without it, a dialog on Windows belonged to no window: it could open
  // behind the app, and the app stayed clickable while it was open.
  void set_parent_window(std::function<ParentWindow()> parent);

  // `start`, when given, is the folder the panel opens in.
  std::optional<std::string> choose_folder(const std::optional<std::string> &start = std::nullopt);

  // Multi-select file picker, no type filter - a matter document can be
  // anything (contract, exhibit, spreadsheet), and Singularity only reads the
  // content of .txt/.md ones today (see Matter/MatterRepository/MatterRepository.hpp's
  // build_document_context()); everything else is still a legitimate
  // reference. Empty (not an error) when the user cancels.
  std::vector<std::string> choose_files();

  // Single-file picker filtered to one file type (e.g. "Model file"/"gguf") -
  // used to choose a local model file the person already has. std::nullopt
  // (not an error) when the user cancels.
  std::optional<std::string> choose_file(const std::string &filter_name,
                                         const std::string &filter_extension);

  // Native Save dialog for one local export file (see matter.exportText in
  // Bridge/NativeBridge/NativeBridge.cpp). `filter_name`/`filter_extension` offer the
  // OS's own single file-type filter (e.g. "Markdown"/"md") and
  // `default_name` seeds the suggested filename - both purely local UX, no
  // effect on what gets written. Empty (not an error) when the user cancels.
  std::optional<std::string> choose_save_path(const std::string &default_name,
                                              const std::string &filter_name,
                                              const std::string &filter_extension);

private:
  bool initialized_ = false;
  std::function<ParentWindow()> parent_;
};

} // namespace singularity::platform
