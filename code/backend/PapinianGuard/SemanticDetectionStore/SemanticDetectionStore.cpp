#include "PapinianGuard/SemanticDetectionStore/SemanticDetectionStore.hpp"

#include "Platform/Sha256/Sha256.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>

namespace singularity::protected_api::papinian {
namespace {

constexpr int kFormatVersion = 2;
constexpr const char *kMagic = "singularity-papinian-detections";
// The model reports a few candidates per byte; far more than this is not an
// entry this build wrote.
constexpr std::size_t kMaximumEntitiesPerByte = 64;

std::string model_key_for(const SemanticModelIdentity &model) {
  return model.adapter_name + "\n" + model.checkpoint + "\n" + model.revision;
}

void write_atomically(const std::filesystem::path &path, const std::string &content) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return;
  static std::atomic<unsigned long long> counter{0};
  std::filesystem::path temporary = path;
  temporary += ".tmp-" + std::to_string(counter.fetch_add(1));
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return;
    output << content;
    if (!output) {
      std::filesystem::remove(temporary, error);
      return;
    }
  }
  std::filesystem::rename(temporary, path, error);
  if (error) std::filesystem::remove(temporary, error);
}

std::mutex &process_store_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::shared_ptr<const SemanticDetectionStore> &process_store_slot() {
  static std::shared_ptr<const SemanticDetectionStore> store;
  return store;
}

} // namespace

SemanticDetectionStore::SemanticDetectionStore(std::filesystem::path directory, SemanticModelIdentity model)
    : directory_(std::move(directory)), model_(std::move(model)), model_key_(model_key_for(model_)) {}

bool SemanticDetectionStore::serves(const SemanticModelIdentity &model) const {
  return model_key_for(model) == model_key_;
}

std::filesystem::path SemanticDetectionStore::entry_path(const std::string &utf8_text) const {
  const std::string digest = platform::sha256_hex_digest(
      "papinian-semantic-detection\n" + std::to_string(kFormatVersion) + "\n" + model_key_ + "\n" + utf8_text);
  return directory_ / digest.substr(0, 2) / (digest + ".json");
}

std::filesystem::path SemanticDetectionStore::marker_path(const std::string &unit_key) const {
  const std::string digest = platform::sha256_hex_digest(
      "papinian-semantic-unit\n" + std::to_string(kFormatVersion) + "\n" + model_key_ + "\n" + unit_key);
  return directory_ / "units" / (digest + ".done");
}

// File format, one record per line, no text and no detected values:
//   singularity-papinian-detections <version> <text bytes> <entity count> <confidence floor>
//   <start> <length> <confidence> <label>
std::optional<std::vector<ModelEntity>> SemanticDetectionStore::load(const std::string &utf8_text,
    double minimum_confidence) const {
  try {
    std::ifstream input(entry_path(utf8_text), std::ios::binary);
    if (!input) return std::nullopt;
    std::string header;
    if (!std::getline(input, header)) return std::nullopt;
    std::istringstream headerStream(header);
    std::string magic;
    int version = 0;
    std::size_t textBytes = 0;
    std::size_t count = 0;
    double floor = 0.0;
    if (!(headerStream >> magic >> version >> textBytes >> count >> floor)) return std::nullopt;
    if (magic != kMagic || version != kFormatVersion || textBytes != utf8_text.size()) return std::nullopt;
    if (!std::isfinite(floor) || floor < 0.0 || floor > 1.0) return std::nullopt;
    // Saved without the detections this reader may still accept: a miss.
    if (floor > minimum_confidence) return std::nullopt;
    if (count > (utf8_text.size() + 16) * kMaximumEntitiesPerByte) return std::nullopt;
    std::vector<ModelEntity> entities;
    entities.reserve(count);
    std::string line;
    for (std::size_t index = 0; index < count; ++index) {
      if (!std::getline(input, line)) return std::nullopt;
      std::istringstream entry(line);
      ModelEntity entity;
      if (!(entry >> entity.start >> entity.length >> entity.confidence)) return std::nullopt;
      if (entry.get() != ' ') return std::nullopt;
      std::getline(entry, entity.label);
      // An entry that does not fit the text it claims to describe is not
      // trusted at all - the model runs instead.
      if (entity.label.empty() || entity.length == 0 || entity.start > utf8_text.size() ||
          entity.length > utf8_text.size() - entity.start || !std::isfinite(entity.confidence)) {
        return std::nullopt;
      }
      entities.push_back(std::move(entity));
    }
    return entities;
  } catch (...) {
    return std::nullopt;
  }
}

bool SemanticDetectionStore::contains(const std::string &utf8_text, double minimum_confidence) const {
  return load(utf8_text, minimum_confidence).has_value();
}

void SemanticDetectionStore::save(const std::string &utf8_text, const std::vector<ModelEntity> &entities,
    double minimum_confidence) const {
  try {
    const double floor = std::isfinite(minimum_confidence) ? std::min(1.0, std::max(0.0, minimum_confidence)) : 0.0;
    std::string body;
    std::size_t kept = 0;
    for (const ModelEntity &entity : entities) {
      if (entity.label.find('\n') != std::string::npos || entity.label.empty()) return;
      // Below the floor no reader this entry serves can accept it.
      if (std::isfinite(entity.confidence) && entity.confidence < floor) continue;
      char confidence[64];
      std::snprintf(confidence, sizeof(confidence), "%.17g", entity.confidence);
      body += std::to_string(entity.start) + " " + std::to_string(entity.length) + " " + confidence + " " +
          entity.label + "\n";
      ++kept;
    }
    char floorText[64];
    std::snprintf(floorText, sizeof(floorText), "%.17g", floor);
    const std::string header = std::string(kMagic) + " " + std::to_string(kFormatVersion) + " " +
        std::to_string(utf8_text.size()) + " " + std::to_string(kept) + " " + floorText + "\n";
    write_atomically(entry_path(utf8_text), header + body);
  } catch (...) {
    // A failed save only means the next send scores this text again.
  }
}

std::size_t SemanticDetectionStore::prune_foreign_entries() const {
  std::size_t removed = 0;
  std::error_code error;
  if (!std::filesystem::is_directory(directory_, error)) return 0;
  const std::string expected = std::string(kMagic) + " " + std::to_string(kFormatVersion) + " ";
  const std::string expectedMark = std::to_string(kFormatVersion);
  for (std::filesystem::recursive_directory_iterator entry(directory_, error), end; !error && entry != end;
       entry.increment(error)) {
    if (!entry->is_regular_file(error)) continue;
    const std::filesystem::path path = entry->path();
    const bool isMark = path.extension() == ".done";
    if (!isMark && path.extension() != ".json") continue;
    std::string firstLine;
    {
      std::ifstream input(path, std::ios::binary);
      if (!input) continue;
      std::getline(input, firstLine);
    }
    const bool current = isMark ? firstLine == expectedMark : firstLine.compare(0, expected.size(), expected) == 0;
    if (current) continue;
    std::error_code removeError;
    if (std::filesystem::remove(path, removeError)) ++removed;
  }
  return removed;
}

bool SemanticDetectionStore::is_marked(const std::string &unit_key) const {
  std::error_code error;
  return std::filesystem::exists(marker_path(unit_key), error);
}

void SemanticDetectionStore::mark(const std::string &unit_key) const {
  write_atomically(marker_path(unit_key), std::to_string(kFormatVersion));
}

void set_process_semantic_detection_store(std::shared_ptr<const SemanticDetectionStore> store) {
  const std::lock_guard<std::mutex> lock(process_store_mutex());
  process_store_slot() = std::move(store);
}

std::shared_ptr<const SemanticDetectionStore> process_semantic_detection_store() {
  const std::lock_guard<std::mutex> lock(process_store_mutex());
  return process_store_slot();
}

} // namespace singularity::protected_api::papinian
