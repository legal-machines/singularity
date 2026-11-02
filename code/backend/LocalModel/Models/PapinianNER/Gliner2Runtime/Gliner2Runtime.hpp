#pragma once

#include "LocalModel/Models/PapinianNER/Gliner2Adapter/Gliner2Adapter.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace singularity::protected_api::papinian {

// Internal UTF-8 byte-offset -> scalar (code point) offset resolution for
// GLiNER2's own returned entity spans - deliberately dependency-light
// (pure string/offset math, no ONNX/model dependency) and exposed here,
// rather than kept file-local in Gliner2Runtime.cpp, so tests can exercise
// the exact production offset-conversion algorithm directly - the same
// reason ResearchTools.hpp exposes validate_search()/validate_open() as
// inline functions instead of keeping them anonymous-namespace internals.
//
// utf8_scalar_offsets() is the one NativeGliner2InferenceEngine::infer()
// (Gliner2Runtime.cpp) actually calls in production: it resolves every
// requested byte offset in ONE linear traversal of `text`, shared across
// all of them, rather than restarting the scan from byte 0 per offset. A
// live Phase 24.1D acceptance run physically proved the naive per-offset
// version (utf8_scalar_offset() below, kept only as the equivalence
// reference these tests compare against) is not a theoretical inefficiency:
// infer() calls it twice per detected entity (start and end), so cost
// scaled with entities x text length, and became the dominant real cost
// once a research-loop prompt - re-protected by Papinian on every
// continuation turn - grew large across several real research iterations.
//
// Both share the identical validation rules: a requested offset past
// text.size() is invalid, and a requested offset landing inside a UTF-8
// continuation byte is invalid - never guessed, never silently corrected.
// Only how the traversal is shared across offsets differs; neither
// function's result depends on GLiNER2's own labels, thresholds, chunking,
// or overlap semantics, none of which either function touches.
inline std::size_t utf8_scalar_offset(const std::string &text, std::size_t byte_offset) {
  if (byte_offset > text.size()) {
    throw std::runtime_error("GLiNER2 returned an out-of-bounds UTF-8 byte offset");
  }
  std::size_t scalars = 0;
  for (std::size_t offset = 0; offset < byte_offset; ++offset) {
    if ((static_cast<unsigned char>(text[offset]) & 0xc0) != 0x80) ++scalars;
  }
  if (byte_offset < text.size() &&
      (static_cast<unsigned char>(text[byte_offset]) & 0xc0) == 0x80) {
    throw std::runtime_error("GLiNER2 returned an offset inside a UTF-8 scalar");
  }
  return scalars;
}

inline std::unordered_map<std::size_t, std::size_t> utf8_scalar_offsets(
    const std::string &text, const std::vector<std::size_t> &byte_offsets) {
  std::vector<std::size_t> sorted_offsets(byte_offsets.begin(), byte_offsets.end());
  std::sort(sorted_offsets.begin(), sorted_offsets.end());
  sorted_offsets.erase(std::unique(sorted_offsets.begin(), sorted_offsets.end()), sorted_offsets.end());
  for (const std::size_t offset : sorted_offsets) {
    if (offset > text.size()) {
      throw std::runtime_error("GLiNER2 returned an out-of-bounds UTF-8 byte offset");
    }
  }

  std::unordered_map<std::size_t, std::size_t> scalar_by_byte_offset;
  scalar_by_byte_offset.reserve(sorted_offsets.size());
  std::size_t scalars = 0;
  std::size_t next_requested = 0;
  for (std::size_t position = 0; position <= text.size() && next_requested < sorted_offsets.size(); ++position) {
    while (next_requested < sorted_offsets.size() && sorted_offsets[next_requested] == position) {
      if (position < text.size() && (static_cast<unsigned char>(text[position]) & 0xc0) == 0x80) {
        throw std::runtime_error("GLiNER2 returned an offset inside a UTF-8 scalar");
      }
      scalar_by_byte_offset[position] = scalars;
      ++next_requested;
    }
    if (position < text.size() && (static_cast<unsigned char>(text[position]) & 0xc0) != 0x80) ++scalars;
  }
  return scalar_by_byte_offset;
}

struct Gliner2ModelArtifact {
  std::filesystem::path relative_path;
  std::string sha256;
};

struct Gliner2ModelPackage {
  std::filesystem::path root;
  std::string checkpoint_revision;
  std::string export_revision;
  std::string variant;
  std::vector<Gliner2ModelArtifact> artifacts;
};

// An engine implementation must eagerly open every model/tokenizer artifact in
// its constructor. The verified package must not be consulted lazily after the
// factory returns, because files could be replaced after verification.
class IGliner2InferenceEngine {
public:
  virtual ~IGliner2InferenceEngine() = default;
  virtual std::vector<Gliner2RuntimeEntity>
  infer(const std::string &utf8_text, const std::vector<std::string> &labels) const = 0;
  virtual std::vector<std::vector<Gliner2RuntimeEntity>>
  infer_batch(const std::vector<std::string> &utf8_texts,
              const std::vector<std::string> &labels,
              std::size_t microbatch_size = 16) const {
    static_cast<void>(microbatch_size);
    std::vector<std::vector<Gliner2RuntimeEntity>> results;
    results.reserve(utf8_texts.size());
    for (const auto &text : utf8_texts) results.push_back(infer(text, labels));
    return results;
  }
};

using Gliner2EngineFactory = std::function<std::shared_ptr<const IGliner2InferenceEngine>(
    const Gliner2ModelPackage &)>;

// Production zero-Python engine. Construction eagerly opens tokenizer.json and
// all eight V2 ONNX sessions through the pinned native GLiNER2 runtime bridge.
std::shared_ptr<const IGliner2InferenceEngine>
make_native_gliner2_engine(const Gliner2ModelPackage &package);

// Verifies the complete local export before constructing the engine, loads it
// once on first use, and reuses it across requests. A verification/load failure
// is sticky for the process lifetime so a request cannot silently switch to a
// different artifact set after the privacy boundary has failed closed.
class VerifiedGliner2Runtime final : public IGliner2Runtime {
public:
  explicit VerifiedGliner2Runtime(Gliner2ModelPackage package);
  VerifiedGliner2Runtime(Gliner2ModelPackage package, Gliner2EngineFactory factory);

  std::vector<Gliner2RuntimeEntity>
  infer(const std::string &utf8_text, const std::vector<std::string> &labels) const override;
  std::vector<std::vector<Gliner2RuntimeEntity>>
  infer_batch(const std::vector<std::string> &utf8_texts,
              const std::vector<std::string> &labels,
              std::size_t microbatch_size = 16) const override;

  bool loaded() const;
  // Releases the loaded engine and clears a sticky load failure so an
  // atomically replaced, newly verified package can be loaded on retry.
  // In-flight inference keeps its own shared engine reference and completes.
  void unload();

private:
  std::shared_ptr<const IGliner2InferenceEngine> engine() const;

  Gliner2ModelPackage package_;
  Gliner2EngineFactory factory_;
  mutable std::mutex mutex_;
  mutable std::shared_ptr<const IGliner2InferenceEngine> engine_;
  mutable std::string failure_;
};

void verify_gliner2_model_package(const Gliner2ModelPackage &package);

// The exact hash-pinned FP16 V2 package selected for the current product
// integration. Keeping this manifest in native code prevents routing from
// accepting an arbitrary model directory or unrecorded weights.
Gliner2ModelPackage pinned_fastino_v2_model_package(const std::filesystem::path &root);

std::size_t gliner2_encoder_forward_calls_total();
std::size_t gliner2_encoder_batches_total();
std::size_t gliner2_downstream_forward_calls_total();

} // namespace singularity::protected_api::papinian
