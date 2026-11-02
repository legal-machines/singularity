#include "Platform/FileDrop/FileDrop.hpp"

#include <nlohmann/json.hpp>

namespace singularity::platform {
namespace {

// JSON is a JavaScript expression: every value arrives as a literal, whatever
// characters a file's name holds. Bytes that are not UTF-8 are replaced rather
// than ending the script.
std::string literal(const nlohmann::json &value) {
  return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // namespace

std::string files_dragging_script(double x, double y) {
  return "window.dispatchEvent(new CustomEvent(\"singularity:files-dragging\", { detail: " +
         literal({{"x", x}, {"y", y}}) + " }));";
}

std::string files_drag_ended_script() {
  return "window.dispatchEvent(new CustomEvent(\"singularity:files-drag-ended\"));";
}

std::string files_dropped_script(const std::vector<DroppedFile> &files, double x, double y) {
  nlohmann::json detail;
  detail["files"] = nlohmann::json::array();
  for (const DroppedFile &file : files) {
    if (file.path.empty()) continue;
    detail["files"].push_back({{"path", file.path}, {"isFolder", file.is_folder}});
  }
  detail["x"] = x;
  detail["y"] = y;
  return "(() => { const dropped = new CustomEvent(\"singularity:files-dropped\", { cancelable: true, detail: " +
         literal(detail) + " }); window.dispatchEvent(dropped); return dropped.defaultPrevented; })()";
}

std::string drop_zones_script() {
  return "JSON.stringify(typeof window.singularityFileDropZones == \"function\" ? window.singularityFileDropZones() : [])";
}

std::vector<DropZone> drop_zones_from_json(const std::string &json) {
  std::vector<DropZone> zones;
  const nlohmann::json read = nlohmann::json::parse(json, nullptr, false);
  if (!read.is_array()) return zones;
  for (const nlohmann::json &zone : read) {
    if (!zone.is_object()) continue;
    const auto number = [&](const char *key) {
      const auto found = zone.find(key);
      return found != zone.end() && found->is_number() ? found->get<double>() : -1.0;
    };
    DropZone place{number("x"), number("y"), number("width"), number("height")};
    if (place.width <= 0 || place.height <= 0) continue;
    zones.push_back(place);
  }
  return zones;
}

bool zone_contains(const std::vector<DropZone> &zones, double x, double y) {
  for (const DropZone &zone : zones) {
    if (x >= zone.x && x <= zone.x + zone.width && y >= zone.y && y <= zone.y + zone.height) return true;
  }
  return false;
}

#if !defined(__APPLE__) && !defined(__linux__) && !defined(_WIN32)
void install_file_drop(void *) {}
#endif

} // namespace singularity::platform
