#pragma once

#include <string>
#include <vector>

namespace singularity::platform {

// One file or folder dropped on the window, as the system names it.
struct DroppedFile {
  std::string path;
  bool is_folder = false;
};

// A place on the page that takes files, in page pixels.
struct DropZone {
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
};

// The window takes a drag of files itself, wherever the page has a place for
// them, and tells the page as it goes (App/services/fileDrop.js reads it):
//   singularity:files-dragging   { x, y }   files are over the window here
//   singularity:files-drag-ended            they left, or were dropped
//   singularity:files-dropped    { files: [{ path, isFolder }], x, y }
// A page never sees a dropped file's path otherwise: the web view gives it
// names only. The dropped event is cancelable; the place that takes the files
// cancels it, and the script's value says whether one did.
std::string files_dragging_script(double x, double y);
std::string files_drag_ended_script();
std::string files_dropped_script(const std::vector<DroppedFile> &files, double x, double y);

// The page's places for files, asked as a drag arrives, and read back.
std::string drop_zones_script();
std::vector<DropZone> drop_zones_from_json(const std::string &json);
bool zone_contains(const std::vector<DropZone> &zones, double x, double y);

// The window's web view takes drags of files as above; every other drag is
// the web view's own. macOS (FileDropMacOS.mm) takes the WKWebView; Windows
// (FileDropWindows.cpp) takes the ICoreWebView2Controller, where a script of
// the shell's follows the drag in the page and hands the dropped files to the
// shell for their paths; Linux (FileDropLinux.cpp) takes the WebKitWebView.
// Where it is not built it does nothing, and the page keeps its file pickers.
void install_file_drop(void *webview_handle);

} // namespace singularity::platform
