#pragma once

#include "LocalModel/Models/PapinianNER/SemanticModel/SemanticModel.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace singularity::protected_api::papinian {

// Durable memory of what the local semantic model detected in a piece of
// text, so the same Project passage is scored by the model once and not on
// every send.
//
// Why this cannot weaken protection: an entry is addressed by the SHA-256 of
// (format version, model identity, exact text), so it is reused only for
// byte-identical text scored by the same pinned model. It stores exactly
// what the model returned (offsets, labels, confidences) - the result a
// fresh run would give - and every later step still runs on each send:
// deterministic detection, Project entity matching, the per-operation
// mapping, both leak checks and restoration. A missing, stale or unreadable
// entry is simply a miss and the model runs.
//
// What it stores: no text and no detected values - only offsets and labels,
// which mean nothing without the passage itself (which already lives in the
// local document cache).
//
// The confidence floor: the model reports every candidate span under every
// label, and nine in ten of those carry a confidence near zero that no
// threshold ever accepts. An entry keeps the detections at or above the floor
// it was saved with and records that floor; it serves a reader only when the
// reader's own floor is not lower. A reader therefore always receives exactly
// the detections a fresh run would give it above its floor - never fewer.
class SemanticDetectionStore {
public:
  SemanticDetectionStore(std::filesystem::path directory, SemanticModelIdentity model);

  [[nodiscard]] std::optional<std::vector<ModelEntity>> load(const std::string &utf8_text,
      double minimum_confidence = 0.0) const;
  void save(const std::string &utf8_text, const std::vector<ModelEntity> &entities,
      double minimum_confidence = 0.0) const;
  [[nodiscard]] bool contains(const std::string &utf8_text, double minimum_confidence = 0.0) const;

  // Removes entries and unit marks written in another format version, which
  // this build can never read again. Returns how many files it removed.
  std::size_t prune_foreign_entries() const;

  // Marks a whole prepared unit (a document version) as already warmed, so a
  // repeated warm-up pass costs one file check instead of hashing every
  // passage again.
  [[nodiscard]] bool is_marked(const std::string &unit_key) const;
  void mark(const std::string &unit_key) const;

  [[nodiscard]] bool serves(const SemanticModelIdentity &model) const;
  [[nodiscard]] const std::filesystem::path &directory() const noexcept { return directory_; }

private:
  [[nodiscard]] std::filesystem::path entry_path(const std::string &utf8_text) const;
  [[nodiscard]] std::filesystem::path marker_path(const std::string &unit_key) const;

  std::filesystem::path directory_;
  SemanticModelIdentity model_;
  std::string model_key_;
};

// The process-wide store the production guard consults. Unset (the default,
// and in every test that does not set it) means no durable reuse at all.
void set_process_semantic_detection_store(std::shared_ptr<const SemanticDetectionStore> store);
[[nodiscard]] std::shared_ptr<const SemanticDetectionStore> process_semantic_detection_store();

} // namespace singularity::protected_api::papinian
