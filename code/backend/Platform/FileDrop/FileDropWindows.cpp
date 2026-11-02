#include "Platform/FileDrop/FileDrop.hpp"
#include "Platform/WindowNavigation/WindowNavigationWindows.hpp"

#include <windows.h>

// WebView2.h needs objbase.h's `interface`, which WIN32_LEAN_AND_MEAN leaves
// out; the webview library includes WebView2.h after it.
#include <objbase.h>

#include <webview/webview.h>

#include "Platform/WebViewCom/WebViewCom.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <vector>

// Files dragged onto the window on Windows (WebView2), the counterpart of
// FileDropMacOS.mm: the same three events for the page, the same places, the
// same cancelable drop (FileDrop.hpp).
//
// WebView2 gives the drag to the page itself and tells it the files' names
// only. A script of the shell's, run in the window's own document, follows the
// drag there as the macOS shell does natively - where the files are, the
// page's places asked for at most four times a second, a drop allowed only on
// one of them - and on a drop hands the shell the dropped File objects
// (chrome.webview.postMessageWithAdditionalObjects). Only the shell can read
// their paths (ICoreWebView2File); it tells the page
// "singularity:files-dropped" with them, as on macOS. A message from any
// document but the window's own page is dropped unread, as at the native
// bridge (WindowNavigationWindows.cpp).

namespace singularity::platform {
namespace {

using Microsoft::WRL::ComPtr;
using webview_com::make_handler;

// What the page posts when files fall on one of its places.
constexpr const char *kDroppedMessage = "singularityFilesDropped";

constexpr const wchar_t *kPageScript = LR"JS((() => {
  if (window.top !== window || !window.chrome || !window.chrome.webview
      || typeof window.chrome.webview.postMessageWithAdditionalObjects !== "function") return;
  const carriesFiles = event => !!event.dataTransfer && Array.from(event.dataTransfer.types || []).includes("Files");
  const placesNow = () => {
    try {
      const zones = typeof window.singularityFileDropZones === "function" ? window.singularityFileDropZones() : [];
      return Array.isArray(zones) ? zones : [];
    } catch (error) {
      return [];
    }
  };
  const within = (zones, x, y) => zones.some(zone => zone && zone.width > 0 && zone.height > 0
    && x >= zone.x && x <= zone.x + zone.width && y >= zone.y && y <= zone.y + zone.height);
  let drag = null;
  const tell = (name, detail) => window.dispatchEvent(detail === undefined
    ? new CustomEvent(name) : new CustomEvent(name, { detail }));
  const follow = event => {
    const now = performance.now();
    if (now - drag.askedAt > 250) {
      drag.zones = placesNow();
      drag.askedAt = now;
    }
    const x = event.clientX;
    const y = event.clientY;
    if (x !== drag.x || y !== drag.y) {
      drag.x = x;
      drag.y = y;
      tell("singularity:files-dragging", { x, y });
    }
    const taken = within(drag.zones, x, y);
    event.dataTransfer.dropEffect = taken ? "copy" : "none";
  };
  const end = () => {
    if (drag === null) return;
    drag = null;
    tell("singularity:files-drag-ended");
  };
  window.addEventListener("dragenter", event => {
    if (drag === null) {
      if (!carriesFiles(event)) return;
      drag = { depth: 0, zones: [], askedAt: 0, x: null, y: null };
    }
    drag.depth += 1;
    event.preventDefault();
    follow(event);
  }, true);
  window.addEventListener("dragover", event => {
    if (drag === null) return;
    event.preventDefault();
    follow(event);
  }, true);
  window.addEventListener("dragleave", event => {
    if (drag === null) return;
    drag.depth -= 1;
    if (drag.depth <= 0) end();
  }, true);
  window.addEventListener("drop", event => {
    if (drag === null) return;
    event.preventDefault();
    const x = event.clientX;
    const y = event.clientY;
    const files = event.dataTransfer.files;
    const taken = within(placesNow(), x, y);
    end();
    if (taken && files && files.length > 0)
      window.chrome.webview.postMessageWithAdditionalObjects(JSON.stringify({ singularityFilesDropped: { x, y } }), files);
  }, true);
})();)JS";

std::string utf8_of(const wchar_t *text) {
  if (text == nullptr || *text == L'\0') return {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (length <= 1) return {};
  std::string result(static_cast<std::size_t>(length - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
  return result;
}

std::wstring wide_of(const std::string &text) {
  if (text.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return {};
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
  return result;
}

// Where the files fell, in the page's pixels, when the message is a drop.
bool drop_point_of(ICoreWebView2WebMessageReceivedEventArgs *args, double &x, double &y) {
  LPWSTR text = nullptr;
  if (FAILED(args->TryGetWebMessageAsString(&text)) || text == nullptr) return false;
  const std::string message = utf8_of(text);
  CoTaskMemFree(text);
  const nlohmann::json read = nlohmann::json::parse(message, nullptr, false);
  if (!read.is_object()) return false;
  const auto drop = read.find(kDroppedMessage);
  if (drop == read.end() || !drop->is_object()) return false;
  const auto at_x = drop->find("x");
  const auto at_y = drop->find("y");
  if (at_x == drop->end() || at_y == drop->end() || !at_x->is_number() || !at_y->is_number()) return false;
  x = at_x->get<double>();
  y = at_y->get<double>();
  return true;
}

// The paths of the File objects the page handed over with the message.
std::vector<DroppedFile> dropped_files_of(ICoreWebView2WebMessageReceivedEventArgs *args) {
  std::vector<DroppedFile> files;
  ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> with_objects;
  ComPtr<ICoreWebView2ObjectCollectionView> objects;
  if (FAILED(args->QueryInterface(IID_PPV_ARGS(&with_objects))) || FAILED(with_objects->get_AdditionalObjects(&objects))
      || !objects)
    return files;
  UINT32 count = 0;
  if (FAILED(objects->get_Count(&count))) return files;
  for (UINT32 index = 0; index < count; ++index) {
    ComPtr<IUnknown> object;
    ComPtr<ICoreWebView2File> file;
    LPWSTR path = nullptr;
    if (FAILED(objects->GetValueAtIndex(index, &object)) || FAILED(object.As(&file)) || FAILED(file->get_Path(&path))
        || path == nullptr)
      continue;
    const DWORD attributes = GetFileAttributesW(path);
    files.push_back({utf8_of(path), attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
    CoTaskMemFree(path);
  }
  return files;
}

} // namespace

void install_file_drop(void *webview_handle) {
  auto *controller = static_cast<ICoreWebView2Controller *>(webview_handle);
  if (controller == nullptr) return;
  ComPtr<ICoreWebView2> web_view;
  if (FAILED(controller->get_CoreWebView2(&web_view)) || !web_view) return;
  // Where the shell cannot follow a drag of files, the page keeps its file
  // pickers: nothing here stops the window loading.
  if (FAILED(web_view->AddScriptToExecuteOnDocumentCreated(kPageScript, nullptr))) {
    std::cerr << "Singularity could not take files dropped on its window; its file pickers still work.\n";
    return;
  }
  EventRegistrationToken token{};
  web_view->add_WebMessageReceived(
      make_handler<ICoreWebView2WebMessageReceivedEventHandler>(
          [](ICoreWebView2 *sender, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
            double x = 0;
            double y = 0;
            if (sender == nullptr || args == nullptr || !drop_point_of(args, x, y)) return S_OK;
            if (!is_from_window_page(args)) return S_OK;
            const std::vector<DroppedFile> files = dropped_files_of(args);
            if (files.empty()) return S_OK;
            sender->ExecuteScript(
                wide_of(files_dropped_script(files, x, y)).c_str(),
                make_handler<ICoreWebView2ExecuteScriptCompletedHandler>([](HRESULT result, LPCWSTR taken) -> HRESULT {
                  if (FAILED(result)) std::cerr << "Singularity: the page could not hear the files dropped on it.\n";
                  (void)taken;
                  return S_OK;
                }).Get());
            return S_OK;
          })
          .Get(),
      &token);
}

} // namespace singularity::platform
