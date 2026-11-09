#include "Platform/MacOSChrome/MacOSChrome.hpp"

#include "Platform/FileDrop/FileDrop.hpp"

#include <cstdlib>
#include <string>
#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <atomic>
#include <utility>

namespace singularity::platform {
namespace {

// Never destroyed: AppKit may ask as the process ends.
std::function<void()> &quit_handler() {
  static auto *handler = new std::function<void()>();
  return *handler;
}

std::atomic<bool> &is_quit_asked() {
  static auto *asked = new std::atomic<bool>(false);
  return *asked;
}

// -[NSApplicationDelegate applicationShouldTerminate:]: the windows leave the
// screen, the handler runs once, and AppKit waits for the process to end.
NSApplicationTerminateReply should_terminate(id, SEL, NSApplication *) {
  hide_application_windows();
  if (is_quit_asked().exchange(true) == false) {
    const std::function<void()> handler = quit_handler();
    if (handler) handler();
  }
  return NSTerminateLater;
}

NSMenuItem *add_item(NSMenu *menu, NSString *title, SEL action,
                     NSString *key_equivalent,
                     NSEventModifierFlags modifiers = 0, id target = nil) {
  NSMenuItem *item =
      [menu addItemWithTitle:title action:action keyEquivalent:key_equivalent];
  item.keyEquivalentModifierMask = modifiers;
  item.target = target;
  return item;
}

void attach_submenu(NSMenu *main_menu, NSMenu *submenu, NSString *title) {
  NSMenuItem *item =
      [main_menu addItemWithTitle:title action:nil keyEquivalent:@""];
  item.submenu = submenu;
}

NSScrollView *find_scroll_view(NSView *view) {
  if ([view isKindOfClass:[NSScrollView class]]) {
    return static_cast<NSScrollView *>(view);
  }
  for (NSView *child in view.subviews) {
    if (NSScrollView *scroll_view = find_scroll_view(child)) {
      return scroll_view;
    }
  }
  return nil;
}

void normalize_standard_window_buttons(NSWindow *window) {
  for (NSNumber *button_type in @[
         @(NSWindowCloseButton),
         @(NSWindowMiniaturizeButton),
         @(NSWindowZoomButton),
       ]) {
    NSButton *button = [window standardWindowButton:
        static_cast<NSWindowButton>(button_type.integerValue)];
    if (button == nil) continue;
    button.imagePosition = NSImageOnly;
    button.imageScaling = NSImageScaleProportionallyDown;
    button.alignment = NSTextAlignmentCenter;
  }
}

// The window's top strip is the top bar's row: 62 high, like the drag region
// AppWindow lays over it and the page's top bar (--titlebar-height in
// code/frontend/styles/tokens.css). AppKit lays the window buttons out for its
// own 32 titlebar, centred 16 down, which left them 10 above the bar's
// commands and its guard post. They stand as far in from the edge as down from
// the top, centred on the strip's line, with AppKit's own spacing between
// them: 24 in and 24 down, the window's --edge, the strip being an --edge
// above and below the buttons' own 14.
constexpr CGFloat window_strip_height = 62;
// Onboarding's locked window uses 20 points of padding around the native button
// group, matching --art-inset in onboarding/painting/Painting.module.css. Keep
// AppKit's button sizes and spacing. When onboarding ends they move into the
// main window's strip.
constexpr CGFloat onboarding_content_inset = 20;

CGFloat strip_height(NSWindow *window) {
  if (NSEqualSizes(window.contentMinSize, window.contentMaxSize)) {
    NSButton *close = [window standardWindowButton:NSWindowCloseButton];
    return 2 * onboarding_content_inset + NSHeight(close.frame);
  }
  return window_strip_height;
}

void place_standard_window_buttons(NSWindow *window) {
  // In full screen the buttons belong to the menu bar's reveal, AppKit's own.
  if ((window.styleMask & NSWindowStyleMaskFullScreen) != 0) return;
  NSButton *close = [window standardWindowButton:NSWindowCloseButton];
  NSButton *miniaturize =
      [window standardWindowButton:NSWindowMiniaturizeButton];
  NSButton *zoom = [window standardWindowButton:NSWindowZoomButton];
  NSView *titlebar = close.superview;
  NSView *container = titlebar.superview;
  if (miniaturize == nil || zoom == nil || container == nil) return;

  // AppKit's spacing, read once while the buttons still stand where it put
  // them.
  static CGFloat spacing = 0;
  if (spacing <= 0) spacing = NSMinX(miniaturize.frame) - NSMinX(close.frame);

  // The views that hold the buttons take the whole strip, so every part of a
  // moved button still takes a press.
  const CGFloat height = strip_height(window);
  NSRect strip = container.frame;
  if (NSHeight(strip) != height) {
    strip.size.height = height;
    strip.origin.y = NSHeight(window.frame) - height;
    container.frame = strip;
  }
  if (NSHeight(titlebar.frame) != height) {
    titlebar.frame = NSMakeRect(NSMinX(titlebar.frame), 0,
                                NSWidth(titlebar.frame), height);
  }

  const CGFloat inset = (height - NSHeight(close.frame)) / 2;
  NSButton *buttons[] = {close, miniaturize, zoom};
  for (int index = 0; index < 3; ++index) {
    const NSPoint origin = NSMakePoint(inset + index * spacing, inset);
    if (!NSEqualPoints(buttons[index].frame.origin, origin)) {
      [buttons[index] setFrameOrigin:origin];
    }
  }
}

// AppKit lays the titlebar out afresh whenever the window changes size, comes
// back from full screen, moves to a screen of another scale, or gains or loses
// focus, and puts the buttons back where it keeps them; they are placed again
// each time. A focus change lays the titlebar out after its notification, so
// the buttons are placed once more on the next turn of the run loop (measured
// on macOS 27: without it they stayed at AppKit's 16 after a focus change).
void keep_standard_window_buttons_placed(NSWindow *window) {
  place_standard_window_buttons(window);
  NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
  for (NSNotificationName name in @[
         NSWindowDidResizeNotification,
         NSWindowDidExitFullScreenNotification,
         NSWindowDidChangeScreenNotification,
         NSWindowDidChangeBackingPropertiesNotification,
         NSWindowDidBecomeKeyNotification,
         NSWindowDidResignKeyNotification,
         NSWindowDidBecomeMainNotification,
         NSWindowDidResignMainNotification,
       ]) {
    [center addObserverForName:name
                        object:window
                         queue:nil
                    usingBlock:^(NSNotification *notification) {
                      (void)notification;
                      place_standard_window_buttons(window);
                      dispatch_async(dispatch_get_main_queue(), ^{
                        place_standard_window_buttons(window);
                      });
                    }];
  }
}

// WebKit paces a WKWebView's rendering updates near 60 frames a second, and
// says so in a feature flag of its own: PreferPageRenderingUpdatesNear60FPSEnabled,
// on by default. On a 120Hz display that halves every animation the interface
// draws - measured here at a mean frame interval of 16.4ms with no dropped
// frames, which is a clean 60 rather than a stutter. The flag has no public
// setter, so it is turned off through WebKit's own feature list, and every step
// is guarded: on a WebKit that does not carry the flag, or renames the private
// selectors, this does nothing and the interface keeps the cadence it had.
void allow_display_frame_rate(WKWebView *webview) {
  if (webview == nil) return;
  WKPreferences *preferences = webview.configuration.preferences;
  const SEL features_selector = NSSelectorFromString(@"_features");
  const SEL set_enabled_selector = NSSelectorFromString(@"_setEnabled:forFeature:");
  const SEL key_selector = NSSelectorFromString(@"key");
  if (![preferences respondsToSelector:set_enabled_selector]) return;

  // WebKit has moved this list between the instance and the class over the
  // years; ask both rather than assume either.
  id features = nil;
  if ([preferences respondsToSelector:features_selector]) {
    features = ((id (*)(id, SEL))objc_msgSend)(preferences, features_selector);
  } else if ([[WKPreferences class] respondsToSelector:features_selector]) {
    features = ((id (*)(Class, SEL))objc_msgSend)([WKPreferences class], features_selector);
  }
  if (![features isKindOfClass:[NSArray class]]) return;

  for (id feature in (NSArray *)features) {
    if (![feature respondsToSelector:key_selector]) continue;
    id key = ((id (*)(id, SEL))objc_msgSend)(feature, key_selector);
    if (![key isKindOfClass:[NSString class]]) continue;
    if (![(NSString *)key isEqualToString:@"PreferPageRenderingUpdatesNear60FPSEnabled"]) continue;
    ((void (*)(id, SEL, BOOL, id))objc_msgSend)(preferences, set_enabled_selector, NO, feature);
    return;
  }
}

// A hidden window's page keeps running, at WebKit's slower pace, instead of
// being suspended: behind other windows, minimized or under a locked screen.
// Suspended (WebKit's default), it heard nothing while hidden - the Inbox, its
// count and a colleague's answer waited until the window was seen again.
void keep_hidden_page_running(WKWebView *webview) {
  if (webview == nil) return;
  if (@available(macOS 14.0, *)) {
    WKInactiveSchedulingPolicy policy = WKInactiveSchedulingPolicyThrottle;
#if !defined(NDEBUG)
    // A development build driven by a test while its window is covered runs
    // its page at full pace (NATIVE_APP_PAGE_ALWAYS_ACTIVE=1).
    if (const char *flag = std::getenv("NATIVE_APP_PAGE_ALWAYS_ACTIVE"); flag != nullptr && std::string(flag) == "1")
      policy = WKInactiveSchedulingPolicyNone;
#endif
    webview.configuration.preferences.inactiveSchedulingPolicy = policy;
  }
}

} // namespace

void hide_application_windows() {
  @autoreleasepool {
    for (NSWindow *window in [NSApplication sharedApplication].windows) {
      [window orderOut:nil];
    }
    // The main thread waits next, so the change is committed to the window
    // server now rather than at a run loop turn that never comes. QuartzCore
    // is loaded with AppKit; it is reached by name, as no link line names it.
    Class transaction = NSClassFromString(@"CATransaction");
    const SEL flush = NSSelectorFromString(@"flush");
    if (transaction != nil && [transaction respondsToSelector:flush]) {
      ((void (*)(Class, SEL))objc_msgSend)(transaction, flush);
    }
  }
}

void install_quit_handler(std::function<void()> quit) {
  @autoreleasepool {
    quit_handler() = std::move(quit);
    NSApplication *application = [NSApplication sharedApplication];
    id delegate = application.delegate;
    if (delegate == nil) return;
    Class delegate_class = object_getClass(delegate);
    const SEL selector = @selector(applicationShouldTerminate:);
    const IMP implementation = reinterpret_cast<IMP>(&should_terminate);
    if (class_addMethod(delegate_class, selector, implementation, "Q@:@") == NO)
      class_replaceMethod(delegate_class, selector, implementation, "Q@:@");
    // AppKit reads what its delegate answers when the delegate is set.
    application.delegate = nil;
    application.delegate = delegate;
  }
}

void configure_macos_chrome(void *window_handle, void *webview_handle,
                            const char *application_name_utf8) {
  @autoreleasepool {
    NSWindow *window = static_cast<NSWindow *>(window_handle);
    window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
    // Closed, the window leaves at once: AppKit's closing animation runs on
    // the main thread, which the closing then holds while work records where
    // it stands, so the window stood frozen mid-animation for five seconds.
    window.animationBehavior = NSWindowAnimationBehaviorNone;
    normalize_standard_window_buttons(window);
    keep_standard_window_buttons_placed(window);
    WKWebView *webview = static_cast<WKWebView *>(webview_handle);
    allow_display_frame_rate(webview);
    keep_hidden_page_running(webview);
    install_file_drop(webview);
    if (NSScrollView *scroll_view = find_scroll_view(webview)) {
      scroll_view.horizontalScrollElasticity = NSScrollElasticityNone;
      scroll_view.verticalScrollElasticity = NSScrollElasticityNone;
    }

    NSApplication *application = [NSApplication sharedApplication];
    // The executable owns AppKit's lifecycle directly instead of entering
    // through NSApplicationMain.  A bundled launch from a terminal, an
    // automation host, or a second explicit `open -n` therefore does not
    // reliably receive Finder's normal activation hand-off.  Keep the native
    // window reachable in every supported production launch path.
    [application setActivationPolicy:NSApplicationActivationPolicyRegular];
    [application activateIgnoringOtherApps:YES];
    NSString *application_name =
        [NSString stringWithUTF8String:application_name_utf8];
    NSMenu *main_menu = [[NSMenu alloc] initWithTitle:@""];

    NSMenu *application_menu =
        [[NSMenu alloc] initWithTitle:application_name];
    add_item(application_menu,
             [@"About " stringByAppendingString:application_name],
             @selector(orderFrontStandardAboutPanel:), @"", 0, application);
    [application_menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *services_item =
        add_item(application_menu, @"Services", nil, @"");
    NSMenu *services_menu = [[NSMenu alloc] initWithTitle:@"Services"];
    services_item.submenu = services_menu;
    application.servicesMenu = services_menu;
    [services_menu release];

    [application_menu addItem:[NSMenuItem separatorItem]];
    add_item(application_menu,
             [@"Hide " stringByAppendingString:application_name],
             @selector(hide:), @"h", NSEventModifierFlagCommand, application);
    add_item(application_menu, @"Hide Others",
             @selector(hideOtherApplications:), @"h",
             NSEventModifierFlagCommand | NSEventModifierFlagOption, application);
    add_item(application_menu, @"Show All",
             @selector(unhideAllApplications:), @"", 0, application);
    [application_menu addItem:[NSMenuItem separatorItem]];
    add_item(application_menu,
             [@"Quit " stringByAppendingString:application_name],
             @selector(terminate:), @"q", NSEventModifierFlagCommand,
             application);
    attach_submenu(main_menu, application_menu, application_name);
    [application_menu release];

    NSMenu *file_menu = [[NSMenu alloc] initWithTitle:@"File"];
    add_item(file_menu, @"Close Window", @selector(performClose:), @"w",
             NSEventModifierFlagCommand, window);
    attach_submenu(main_menu, file_menu, @"File");
    [file_menu release];

    NSMenu *edit_menu = [[NSMenu alloc] initWithTitle:@"Edit"];
    add_item(edit_menu, @"Undo", @selector(undo:), @"z",
             NSEventModifierFlagCommand);
    add_item(edit_menu, @"Redo", @selector(redo:), @"z",
             NSEventModifierFlagCommand | NSEventModifierFlagShift);
    [edit_menu addItem:[NSMenuItem separatorItem]];
    add_item(edit_menu, @"Cut", @selector(cut:), @"x",
             NSEventModifierFlagCommand);
    add_item(edit_menu, @"Copy", @selector(copy:), @"c",
             NSEventModifierFlagCommand);
    add_item(edit_menu, @"Paste", @selector(paste:), @"v",
             NSEventModifierFlagCommand);
    add_item(edit_menu, @"Select All", @selector(selectAll:), @"a",
             NSEventModifierFlagCommand);
    attach_submenu(main_menu, edit_menu, @"Edit");
    [edit_menu release];

    NSMenu *view_menu = [[NSMenu alloc] initWithTitle:@"View"];
#if !defined(NDEBUG)
    // A native action also works when the page has failed or blocks browser
    // shortcuts. It is available on every Debug page, including onboarding.
    add_item(view_menu, @"Reload Page", @selector(reload:), @"r",
             NSEventModifierFlagCommand, webview);
    [view_menu addItem:[NSMenuItem separatorItem]];
#endif
    add_item(view_menu, @"Enter Full Screen", @selector(toggleFullScreen:), @"f",
             NSEventModifierFlagCommand | NSEventModifierFlagControl, window);
    attach_submenu(main_menu, view_menu, @"View");
    [view_menu release];

    NSMenu *window_menu = [[NSMenu alloc] initWithTitle:@"Window"];
    add_item(window_menu, @"Minimize", @selector(performMiniaturize:), @"m",
             NSEventModifierFlagCommand, window);
    add_item(window_menu, @"Zoom", @selector(performZoom:), @"", 0, window);
    [window_menu addItem:[NSMenuItem separatorItem]];
    add_item(window_menu, @"Bring All to Front", @selector(arrangeInFront:), @"",
             0, application);
    attach_submenu(main_menu, window_menu, @"Window");
    application.windowsMenu = window_menu;
    [window_menu release];

    application.mainMenu = main_menu;
    [main_menu release];
  }
}

} // namespace singularity::platform
