// Compiled with ARC (deploy/backend/CMakeLists.txt).
#include "Platform/FileDrop/FileDrop.hpp"

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <os/log.h>

#include <memory>

namespace singularity::platform {
namespace {

// The window's web view, and WebKit's own drag methods, which each method
// below calls for every drag that carries no files and for any other view.
__weak WKWebView *drop_web_view = nil;
IMP webkit_entered = nullptr;
IMP webkit_updated = nullptr;
IMP webkit_exited = nullptr;
IMP webkit_prepare = nullptr;
IMP webkit_perform = nullptr;
IMP webkit_conclude = nullptr;

os_log_t drop_log() {
  static os_log_t log = os_log_create("org.legalmachines.workspace", "file-drop");
  return log;
}

// One drag of files, from its arrival to its end. Main thread only.
struct FilesDrag {
  bool active = false;
  std::vector<DropZone> zones;
  bool asking = false;
  CFAbsoluteTime asked_at = 0;
  NSPoint told = NSMakePoint(-1, -1);
};
FilesDrag drag;

template <typename Result>
Result call_webkit(IMP implementation, id self, SEL command, id<NSDraggingInfo> sender) {
  return reinterpret_cast<Result (*)(id, SEL, id<NSDraggingInfo>)>(implementation)(self, command, sender);
}

// Only the window's web view takes files itself.
bool is_drop_view(id self) {
  return self != nil && self == drop_web_view;
}

bool carries_files(id<NSDraggingInfo> sender) {
  NSPasteboard *board = sender.draggingPasteboard;
  return [board canReadObjectForClasses:@[ NSURL.class ] options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}]
      || [board canReadObjectForClasses:@[ NSFilePromiseReceiver.class ] options:nil];
}

// Where the drag is, in the page's own pixels.
NSPoint page_point(WKWebView *web_view, id<NSDraggingInfo> sender) {
  NSPoint point = [web_view convertPoint:sender.draggingLocation fromView:nil];
  if (!web_view.isFlipped) point.y = NSHeight(web_view.bounds) - point.y;
  if (@available(macOS 11.0, *)) {
    if (web_view.pageZoom > 0) {
      point.x /= web_view.pageZoom;
      point.y /= web_view.pageZoom;
    }
  }
  return point;
}

void tell_page(WKWebView *web_view, const std::string &script, void (^heard)(id) = nil) {
  [web_view evaluateJavaScript:[NSString stringWithUTF8String:script.c_str()]
             completionHandler:^(id result, NSError *error) {
               if (error != nil) {
                 os_log_error(drop_log(), "the page could not hear the drag: %{public}@", error.localizedDescription);
               }
               if (heard != nil) heard(result);
             }];
}

// The page's places for files: asked as the drag arrives and again while it
// moves, since a place may scroll, at most four times a second.
void ask_zones(WKWebView *web_view) {
  if (drag.asking) return;
  drag.asking = true;
  drag.asked_at = CFAbsoluteTimeGetCurrent();
  tell_page(web_view, drop_zones_script(), ^(id result) {
    drag.asking = false;
    if (!drag.active) return;
    drag.zones = [result isKindOfClass:NSString.class] ? drop_zones_from_json([(NSString *)result UTF8String])
                                                       : std::vector<DropZone>{};
  });
}

NSDragOperation follow(WKWebView *web_view, id<NSDraggingInfo> sender) {
  const NSPoint point = page_point(web_view, sender);
  if (CFAbsoluteTimeGetCurrent() - drag.asked_at > 0.25) ask_zones(web_view);
  if (!NSEqualPoints(point, drag.told)) {
    drag.told = point;
    tell_page(web_view, files_dragging_script(point.x, point.y));
  }
  return zone_contains(drag.zones, point.x, point.y) ? NSDragOperationCopy : NSDragOperationNone;
}

void end_drag(WKWebView *web_view) {
  if (!drag.active) return;
  drag = FilesDrag{};
  tell_page(web_view, files_drag_ended_script());
}

void tell_dropped(WKWebView *web_view, const std::vector<DroppedFile> &files, NSPoint point) {
  os_log(drop_log(), "files dropped: %lu at %.0f, %.0f", static_cast<unsigned long>(files.size()), point.x, point.y);
  tell_page(web_view, files_dropped_script(files, point.x, point.y), ^(id taken) {
    os_log(drop_log(), "the page %{public}s them", [taken respondsToSelector:@selector(boolValue)] && [taken boolValue]
                                                         ? "took"
                                                         : "did not take");
  });
}

std::vector<DroppedFile> files_at(NSArray<NSURL *> *urls) {
  std::vector<DroppedFile> files;
  for (NSURL *url in urls) {
    if (!url.isFileURL || url.path == nil) continue;
    NSNumber *folder = nil;
    [url getResourceValue:&folder forKey:NSURLIsDirectoryKey error:nil];
    files.push_back({url.path.UTF8String, folder.boolValue == YES});
  }
  return files;
}

// Files another application only promises - an attachment dragged from Mail
// or Outlook - are written here first, one folder per drop.
NSURL *received_files_folder() {
  NSURL *caches = [NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
  NSString *identifier = NSBundle.mainBundle.bundleIdentifier;
  NSString *application = identifier != nil ? identifier : @"Singularity";
  NSURL *folder = [[[caches URLByAppendingPathComponent:application] URLByAppendingPathComponent:@"Received Files"]
      URLByAppendingPathComponent:NSUUID.UUID.UUIDString];
  [NSFileManager.defaultManager createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:nil];
  return folder;
}

struct Received {
  std::vector<DroppedFile> files;
  int generation = 0;
};

void deliver(WKWebView *web_view, id<NSDraggingInfo> sender, NSPoint point) {
  NSPasteboard *board = sender.draggingPasteboard;
  const std::vector<DroppedFile> carried = files_at(
      [board readObjectsForClasses:@[ NSURL.class ] options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}]);
  if (!carried.empty()) {
    tell_dropped(web_view, carried, point);
    return;
  }
  NSArray<NSFilePromiseReceiver *> *promises = [board readObjectsForClasses:@[ NSFilePromiseReceiver.class ] options:nil];
  if (promises.count == 0) return;
  // Each promised file arrives on its own; the page hears them together once
  // none has come for a moment.
  NSURL *folder = received_files_folder();
  NSOperationQueue *queue = [NSOperationQueue new];
  auto received = std::make_shared<Received>();
  for (NSFilePromiseReceiver *receiver in promises) {
    [receiver receivePromisedFilesAtDestination:folder
                                        options:@{}
                                 operationQueue:queue
                                         reader:^(NSURL *file, NSError *error) {
                                           dispatch_async(dispatch_get_main_queue(), ^{
                                             if (error != nil || file.path == nil) {
                                               os_log_error(drop_log(), "a promised file did not arrive");
                                               return;
                                             }
                                             received->files.push_back({file.path.UTF8String, false});
                                             const int generation = ++received->generation;
                                             dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 3),
                                                            dispatch_get_main_queue(), ^{
                                                              if (generation != received->generation) return;
                                                              tell_dropped(web_view, received->files, point);
                                                            });
                                           });
                                         }];
  }
}

NSDragOperation dragging_entered(id self, SEL command, id<NSDraggingInfo> sender) {
  const NSDragOperation offered = call_webkit<NSDragOperation>(webkit_entered, self, command, sender);
  if (!is_drop_view(self)) return offered;
  drag = FilesDrag{};
  if (!carries_files(sender)) return offered;
  drag.active = true;
  os_log(drop_log(), "files arrive over the window");
  return follow(static_cast<WKWebView *>(self), sender);
}

NSDragOperation dragging_updated(id self, SEL command, id<NSDraggingInfo> sender) {
  const NSDragOperation offered = call_webkit<NSDragOperation>(webkit_updated, self, command, sender);
  if (!is_drop_view(self) || !drag.active) return offered;
  return follow(static_cast<WKWebView *>(self), sender);
}

void dragging_exited(id self, SEL command, id<NSDraggingInfo> sender) {
  call_webkit<void>(webkit_exited, self, command, sender);
  if (is_drop_view(self)) end_drag(static_cast<WKWebView *>(self));
}

BOOL prepare_for_drag_operation(id self, SEL command, id<NSDraggingInfo> sender) {
  if (!is_drop_view(self) || !drag.active) return call_webkit<BOOL>(webkit_prepare, self, command, sender);
  const NSPoint point = page_point(static_cast<WKWebView *>(self), sender);
  return zone_contains(drag.zones, point.x, point.y);
}

// A drop of files is the page's, by path: WebKit hears the drag leave rather
// than opening a file in place of Singularity or writing its name into a
// field.
BOOL perform_drag_operation(id self, SEL command, id<NSDraggingInfo> sender) {
  if (!is_drop_view(self) || !drag.active) return call_webkit<BOOL>(webkit_perform, self, command, sender);
  WKWebView *web_view = static_cast<WKWebView *>(self);
  const NSPoint point = page_point(web_view, sender);
  call_webkit<void>(webkit_exited, self, @selector(draggingExited:), sender);
  deliver(web_view, sender, point);
  return YES;
}

void conclude_drag_operation(id self, SEL command, id<NSDraggingInfo> sender) {
  if (!is_drop_view(self) || !drag.active) {
    call_webkit<void>(webkit_conclude, self, command, sender);
    return;
  }
  end_drag(static_cast<WKWebView *>(self));
}

} // namespace

// WebKit's drag methods are taken over on WKWebView itself, not by making the
// web view an instance of a class made here: AppKit keeps per-class facts
// about a view (its corner radii among them) and stopped the app when a
// window holding such a view was moved. The methods act only for the window's
// web view and hand every other drag back to WebKit.
static void take_over(SEL selector, IMP implementation, IMP *webkit) {
  Class web_view_class = WKWebView.class;
  Method method = class_getInstanceMethod(web_view_class, selector);
  if (method == nullptr) return;
  *webkit = method_getImplementation(method);
  // A method WKWebView only inherits is added to it, never changed for every
  // view; its own is replaced.
  if (!class_addMethod(web_view_class, selector, implementation, method_getTypeEncoding(method))) {
    *webkit = method_setImplementation(method, implementation);
  }
}

void install_file_drop(void *webview_handle) {
  WKWebView *web_view = (__bridge WKWebView *)webview_handle;
  if (web_view == nil || drop_web_view != nil) return;
  drop_web_view = web_view;
  if (webkit_entered != nullptr) return;
  take_over(@selector(draggingEntered:), reinterpret_cast<IMP>(dragging_entered), &webkit_entered);
  take_over(@selector(draggingUpdated:), reinterpret_cast<IMP>(dragging_updated), &webkit_updated);
  take_over(@selector(draggingExited:), reinterpret_cast<IMP>(dragging_exited), &webkit_exited);
  take_over(@selector(prepareForDragOperation:), reinterpret_cast<IMP>(prepare_for_drag_operation), &webkit_prepare);
  take_over(@selector(performDragOperation:), reinterpret_cast<IMP>(perform_drag_operation), &webkit_perform);
  take_over(@selector(concludeDragOperation:), reinterpret_cast<IMP>(conclude_drag_operation), &webkit_conclude);
}

} // namespace singularity::platform
