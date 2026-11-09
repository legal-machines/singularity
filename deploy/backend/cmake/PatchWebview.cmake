# Patches webview 0.12.0 (FetchDeps.cmake runs it as webview's PATCH_COMMAND,
# in webview's source folder) so that the embedding application names
# WebView2's user data folder.
#
# webview creates WebView2's user data folder, the window's browser cache and
# the page's own storage, at %APPDATA%\<program file name>, which is the roaming
# profile, and offers no way to choose another. Microsoft's guidance is the
# folder where the app keeps the rest of its data, never a network drive: a
# roaming profile is often redirected to one. Singularity keeps it in
# %LOCALAPPDATA%\Singularity\WebView2.
#
# The patch adds webview::mswebview2_user_data_folder(), which the application
# sets before it creates a webview. Left empty, webview's own choice stands.
# Only the Windows (WebView2) part of the header changes, but the script runs
# wherever webview is fetched, and may run again at a later configure. It leaves
# a header it has already patched as it is, and fails on any webview but
# 0.12.0. FetchDeps.cmake then checks that the header carries the patch.

cmake_policy(VERSION 3.24)

set(header "core/include/webview/webview.h")
if(NOT EXISTS "${header}")
  message(FATAL_ERROR "PatchWebview.cmake: ${header} is not in ${CMAKE_CURRENT_SOURCE_DIR}.")
endif()
file(READ "${header}" text)
# A Windows checkout may have CRLF line ends; the patch is written for LF.
string(REPLACE "\r\n" "\n" text "${text}")

# Written for webview 0.12.0 alone: another version stops the configure, so a
# webview update means looking at this patch again, never carrying it blindly.
foreach(version_line "MAJOR 0" "MINOR 12" "PATCH 0" "PRE_RELEASE \"\"")
  string(FIND "${text}" "\n#define WEBVIEW_VERSION_${version_line}\n" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "PatchWebview.cmake: webview is not 0.12.0 (no WEBVIEW_VERSION_${version_line}). "
      "Check whether the new version names WebView2's user data folder itself, then update or drop this patch.")
  endif()
endforeach()

set(marker "Singularity patch (deploy/backend/cmake/PatchWebview.cmake)")
string(FIND "${text}" "${marker}" patched)
if(NOT patched EQUAL -1)
  return()
endif()

# Replaces `from` with `to` in `text`, where `from` occurs exactly once.
function(replace_once from to)
  string(FIND "${text}" "${from}" first)
  string(FIND "${text}" "${from}" last REVERSE)
  if(first EQUAL -1 OR NOT first EQUAL last)
    message(FATAL_ERROR "PatchWebview.cmake: webview's header is not the 0.12.0 it patches; "
      "this text is not in it exactly once:\n${from}")
  endif()
  string(REPLACE "${from}" "${to}" replaced "${text}")
  set(text "${replaced}" PARENT_SCOPE)
endfunction()

# 1. The folder the application names, beside the WebView2 implementation.
replace_once([==[
namespace webview {
namespace detail {

using msg_cb_t = std::function<void(const std::string)>;
]==] [==[
namespace webview {

// Singularity patch (deploy/backend/cmake/PatchWebview.cmake): WebView2's user
// data folder, named by the application before it creates a webview. Empty,
// webview's own choice stands: %APPDATA%\<program file name>.
inline std::wstring &mswebview2_user_data_folder() {
  static std::wstring folder;
  return folder;
}

} // namespace webview

namespace webview {
namespace detail {

using msg_cb_t = std::function<void(const std::string)>;
]==])

# 2. The webview uses that folder when it is named.
replace_once([==[
    wchar_t currentExePath[MAX_PATH];
    GetModuleFileNameW(nullptr, currentExePath, MAX_PATH);
    wchar_t *currentExeName = PathFindFileNameW(currentExePath);

    wchar_t dataPath[MAX_PATH];
    if (!SUCCEEDED(
            SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, dataPath))) {
      return error_info{WEBVIEW_ERROR_UNSPECIFIED, "SHGetFolderPathW failed"};
    }
    wchar_t userDataFolder[MAX_PATH];
    PathCombineW(userDataFolder, dataPath, currentExeName);
]==] [==[
    std::wstring userDataFolder = mswebview2_user_data_folder();
    if (userDataFolder.empty()) {
      wchar_t currentExePath[MAX_PATH];
      GetModuleFileNameW(nullptr, currentExePath, MAX_PATH);
      wchar_t *currentExeName = PathFindFileNameW(currentExePath);

      wchar_t dataPath[MAX_PATH];
      if (!SUCCEEDED(
              SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, dataPath))) {
        return error_info{WEBVIEW_ERROR_UNSPECIFIED, "SHGetFolderPathW failed"};
      }
      wchar_t defaultFolder[MAX_PATH];
      PathCombineW(defaultFolder, dataPath, currentExeName);
      userDataFolder = defaultFolder;
    }
]==])

replace_once([==[
          nullptr, userDataFolder, nullptr, m_com_handler);
]==] [==[
          nullptr, userDataFolder.c_str(), nullptr, m_com_handler);
]==])

file(WRITE "${header}" "${text}")
message(STATUS "webview: patched so the application names WebView2's user data folder")
