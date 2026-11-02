#include "Platform/NativeDialogs/NativeDialogs.hpp"

#include <nfd.h>

#include <stdexcept>
#include <utility>

namespace singularity::platform {

namespace {

// The parent window as NFD names it; unset where there is none.
nfdwindowhandle_t native_parent(const std::function<ParentWindow()> &parent) {
  nfdwindowhandle_t handle{};
  if (!parent) return handle;
  const ParentWindow window = parent();
  if (window.handle == nullptr) return handle;
  switch (window.system) {
  case ParentWindow::System::windows:
    handle.type = NFD_WINDOW_HANDLE_TYPE_WINDOWS;
    break;
  case ParentWindow::System::cocoa:
    handle.type = NFD_WINDOW_HANDLE_TYPE_COCOA;
    break;
  case ParentWindow::System::x11:
    handle.type = NFD_WINDOW_HANDLE_TYPE_X11;
    break;
  case ParentWindow::System::none:
    return handle;
  }
  handle.handle = window.handle;
  return handle;
}

} // namespace

void NativeDialogs::set_parent_window(std::function<ParentWindow()> parent) {
  parent_ = std::move(parent);
}

NativeDialogs::NativeDialogs() {
  if (NFD_Init() != NFD_OKAY) {
    const auto *error = NFD_GetError();
    throw std::runtime_error(error == nullptr ? "Unable to initialize NFD"
                                              : error);
  }
  initialized_ = true;
}

NativeDialogs::~NativeDialogs() {
  if (initialized_) {
    NFD_Quit();
  }
}

std::optional<std::string> NativeDialogs::choose_folder(const std::optional<std::string> &start) {
  nfdu8char_t *selected_path = nullptr;
  nfdpickfolderu8args_t arguments{};
  arguments.parentWindow = native_parent(parent_);
  if (start.has_value() == true && start->empty() == false) arguments.defaultPath = start->c_str();
  const auto result = NFD_PickFolderU8_With(&selected_path, &arguments);
  if (result == NFD_OKAY) {
    const std::string path(selected_path);
    NFD_FreePathU8(selected_path);
    return path;
  }
  if (result == NFD_CANCEL) {
    return std::nullopt;
  }
  const auto *error = NFD_GetError();
  throw std::runtime_error(error == nullptr ? "Unable to open folder picker"
                                            : error);
}

std::vector<std::string> NativeDialogs::choose_files() {
  const nfdpathset_t *paths = nullptr;
  nfdopendialogu8args_t arguments{};
  arguments.parentWindow = native_parent(parent_);
  const auto result = NFD_OpenDialogMultipleU8_With(&paths, &arguments);

  std::vector<std::string> selected;
  if (result == NFD_CANCEL) {
    return selected;
  }
  if (result != NFD_OKAY) {
    const auto *error = NFD_GetError();
    throw std::runtime_error(error == nullptr ? "Unable to open file picker"
                                              : error);
  }

  nfdpathsetsize_t count = 0;
  NFD_PathSet_GetCount(paths, &count);
  selected.reserve(count);
  for (nfdpathsetsize_t index = 0; index < count; ++index) {
    nfdu8char_t *path = nullptr;
    if (NFD_PathSet_GetPathU8(paths, index, &path) == NFD_OKAY) {
      selected.emplace_back(path);
      NFD_PathSet_FreePathU8(path);
    }
  }
  NFD_PathSet_Free(paths);
  return selected;
}

std::optional<std::string> NativeDialogs::choose_file(const std::string &filter_name,
                                                      const std::string &filter_extension) {
  nfdu8char_t *selected_path = nullptr;
  const nfdu8filteritem_t filter{filter_name.c_str(), filter_extension.c_str()};
  nfdopendialogu8args_t arguments{};
  arguments.filterList = &filter;
  arguments.filterCount = 1;
  arguments.parentWindow = native_parent(parent_);
  const auto result = NFD_OpenDialogU8_With(&selected_path, &arguments);
  if (result == NFD_OKAY) {
    const std::string path(selected_path);
    NFD_FreePathU8(selected_path);
    return path;
  }
  if (result == NFD_CANCEL) return std::nullopt;
  const auto *error = NFD_GetError();
  throw std::runtime_error(error == nullptr ? "Unable to open file picker" : error);
}

std::optional<std::string> NativeDialogs::choose_save_path(const std::string &default_name,
                                                            const std::string &filter_name,
                                                            const std::string &filter_extension) {
  nfdu8char_t *selected_path = nullptr;
  const nfdu8filteritem_t filter{filter_name.c_str(), filter_extension.c_str()};
  nfdsavedialogu8args_t arguments{};
  arguments.filterList = &filter;
  arguments.filterCount = 1;
  arguments.defaultName = default_name.c_str();
  arguments.parentWindow = native_parent(parent_);
  const auto result = NFD_SaveDialogU8_With(&selected_path, &arguments);
  if (result == NFD_OKAY) {
    const std::string path(selected_path);
    NFD_FreePathU8(selected_path);
    return path;
  }
  if (result == NFD_CANCEL) {
    return std::nullopt;
  }
  const auto *error = NFD_GetError();
  throw std::runtime_error(error == nullptr ? "Unable to open save dialog"
                                            : error);
}

} // namespace singularity::platform
