// Linux, GTK 3 and WebKitGTK (deploy/backend/CMakeLists.txt, the Linux
// sources). The window takes a drag of files itself, as FileDropMacOS.mm does
// on macOS, and tells the page by the same events (FileDrop.hpp). Every drag
// that carries no files stays WebKit's own.
#include "Platform/FileDrop/FileDrop.hpp"

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <iostream>
#include <string>
#include <vector>

namespace singularity::platform {
namespace {

// One drag of files, from its arrival to its end. GTK's main thread only.
struct FilesDrag {
  bool active = false;
  // The files were let go over a place that takes them, and their names are
  // on their way (drag-data-received).
  bool dropping = false;
  std::vector<DropZone> zones;
  bool asking = false;
  gint64 asked_at = 0;
  double told_x = -1;
  double told_y = -1;
  double drop_x = 0;
  double drop_y = 0;
};
FilesDrag drag;

GdkAtom uri_list() {
  static const GdkAtom atom = gdk_atom_intern_static_string("text/uri-list");
  return atom;
}

bool carries_files(GdkDragContext *context) {
  for (GList *target = gdk_drag_context_list_targets(context); target != nullptr; target = target->next)
    if (GDK_POINTER_TO_ATOM(target->data) == uri_list()) return true;
  return false;
}

// Where the drag is, in the page's own pixels.
void page_point(WebKitWebView *web_view, gint x, gint y, double &page_x, double &page_y) {
  const double zoom = webkit_web_view_get_zoom_level(web_view);
  page_x = zoom > 0 ? x / zoom : x;
  page_y = zoom > 0 ? y / zoom : y;
}

using Heard = void (*)(const std::string &result);

void evaluated(GObject *source, GAsyncResult *result, gpointer data) {
  const auto heard = reinterpret_cast<Heard>(data);
  GError *error = nullptr;
  JSCValue *value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error);
  std::string text;
  if (error != nullptr) {
    std::cerr << "Singularity: the page could not hear the drag: " << error->message << "\n";
    g_error_free(error);
  } else if (value != nullptr) {
    if (jsc_value_is_string(value) || jsc_value_is_boolean(value)) {
      gchar *written = jsc_value_to_string(value);
      text = written;
      g_free(written);
    }
    g_object_unref(value);
  }
  if (heard != nullptr) heard(text);
}

void tell_page(WebKitWebView *web_view, const std::string &script, Heard heard = nullptr) {
  webkit_web_view_evaluate_javascript(web_view, script.c_str(), -1, nullptr, nullptr, nullptr, evaluated,
      reinterpret_cast<gpointer>(heard));
}

// The page's places for files: asked as the drag arrives and again while it
// moves, since a place may scroll, at most four times a second.
void ask_zones(WebKitWebView *web_view) {
  if (drag.asking) return;
  drag.asking = true;
  drag.asked_at = g_get_monotonic_time();
  tell_page(web_view, drop_zones_script(), [](const std::string &result) {
    drag.asking = false;
    if (drag.active) drag.zones = drop_zones_from_json(result);
  });
}

void end_drag(WebKitWebView *web_view) {
  if (!drag.active) return;
  drag = FilesDrag{};
  tell_page(web_view, files_drag_ended_script());
}

gboolean on_motion(GtkWidget *widget, GdkDragContext *context, gint x, gint y, guint time, gpointer) {
  if (!drag.active && !carries_files(context)) return FALSE;
  auto *web_view = WEBKIT_WEB_VIEW(widget);
  if (!drag.active) {
    drag = FilesDrag{};
    drag.active = true;
    ask_zones(web_view);
  }
  double page_x = 0;
  double page_y = 0;
  page_point(web_view, x, y, page_x, page_y);
  if (g_get_monotonic_time() - drag.asked_at > 250000) ask_zones(web_view);
  if (page_x != drag.told_x || page_y != drag.told_y) {
    drag.told_x = page_x;
    drag.told_y = page_y;
    tell_page(web_view, files_dragging_script(page_x, page_y));
  }
  gdk_drag_status(context,
      zone_contains(drag.zones, page_x, page_y) ? GDK_ACTION_COPY : static_cast<GdkDragAction>(0), time);
  return TRUE;
}

// GTK sends drag-leave just before drag-drop, so the drag ends only once no
// drop followed the leave.
gboolean end_after_leave(gpointer data) {
  if (drag.active && !drag.dropping) end_drag(WEBKIT_WEB_VIEW(data));
  g_object_unref(data);
  return G_SOURCE_REMOVE;
}

void on_leave(GtkWidget *widget, GdkDragContext *, guint, gpointer) {
  if (!drag.active || drag.dropping) return;
  g_idle_add(end_after_leave, g_object_ref(widget));
}

gboolean on_drop(GtkWidget *widget, GdkDragContext *context, gint x, gint y, guint time, gpointer) {
  if (!drag.active) return FALSE;
  auto *web_view = WEBKIT_WEB_VIEW(widget);
  double page_x = 0;
  double page_y = 0;
  page_point(web_view, x, y, page_x, page_y);
  if (!zone_contains(drag.zones, page_x, page_y)) {
    gtk_drag_finish(context, FALSE, FALSE, time);
    end_drag(web_view);
    return TRUE;
  }
  drag.dropping = true;
  drag.drop_x = page_x;
  drag.drop_y = page_y;
  gtk_drag_get_data(widget, context, uri_list(), time);
  return TRUE;
}

void on_data(GtkWidget *widget, GdkDragContext *context, gint, gint, GtkSelectionData *data, guint, guint time,
    gpointer) {
  if (!drag.dropping) return;
  g_signal_stop_emission_by_name(widget, "drag-data-received");
  std::vector<DroppedFile> files;
  gchar **uris = gtk_selection_data_get_uris(data);
  for (gchar **uri = uris; uris != nullptr && *uri != nullptr; ++uri) {
    gchar *path = g_filename_from_uri(*uri, nullptr, nullptr);
    if (path == nullptr) continue;
    files.push_back({path, g_file_test(path, G_FILE_TEST_IS_DIR) != FALSE});
    g_free(path);
  }
  g_strfreev(uris);
  gtk_drag_finish(context, files.empty() ? FALSE : TRUE, FALSE, time);
  auto *web_view = WEBKIT_WEB_VIEW(widget);
  if (!files.empty()) tell_page(web_view, files_dropped_script(files, drag.drop_x, drag.drop_y));
  end_drag(web_view);
}

} // namespace

void install_file_drop(void *webview_handle) {
  if (webview_handle == nullptr || !WEBKIT_IS_WEB_VIEW(webview_handle)) return;
  // Connected before WebKit's own handlers run: a drag of files stops there,
  // and every other drag goes on to WebKit.
  GtkWidget *widget = GTK_WIDGET(webview_handle);
  g_signal_connect(widget, "drag-motion", G_CALLBACK(on_motion), nullptr);
  g_signal_connect(widget, "drag-leave", G_CALLBACK(on_leave), nullptr);
  g_signal_connect(widget, "drag-drop", G_CALLBACK(on_drop), nullptr);
  g_signal_connect(widget, "drag-data-received", G_CALLBACK(on_data), nullptr);
}

} // namespace singularity::platform
