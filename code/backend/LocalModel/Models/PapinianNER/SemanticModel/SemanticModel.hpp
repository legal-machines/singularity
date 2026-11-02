#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace singularity::protected_api::papinian {

struct SemanticModelIdentity {
  std::string adapter_name;
  std::string checkpoint;
  std::string revision;
  std::string attribution;
  bool legal_machines_fine_tuned = false;
};

struct ModelEntity {
  // UTF-8 byte offsets into the exact input string. A runtime that reports
  // Unicode code-point offsets must convert them before crossing this boundary.
  std::size_t start = 0;
  std::size_t length = 0;
  std::string label;
  double confidence = 0.0;
};

struct SemanticLeaf {
  std::string identity;
  std::string text;
  // Text the person wrote themselves in this request, as opposed to text taken
  // from a document or written by Singularity. It changes nothing about detection;
  // it decides only which spelling of a name the answer comes back in (see
  // RequestMappingEntry::original).
  bool user_authored = false;
  // Text a model wrote in an earlier answer of this chat (HISTORY), read back.
  // Like user_authored, it changes nothing about detection; it records where a
  // writing was met.
  bool model_authored = false;
};

struct SemanticLeafDetection {
  std::string identity;
  std::vector<ModelEntity> entities;
};

class ISemanticModelAdapter {
public:
  virtual ~ISemanticModelAdapter() = default;
  virtual SemanticModelIdentity identity() const = 0;
  virtual std::vector<ModelEntity> detect(const std::string &utf8_text) const = 0;
  virtual std::vector<SemanticLeafDetection>
  detect_batch(const std::vector<SemanticLeaf> &leaves) const {
    std::vector<SemanticLeafDetection> results;
    results.reserve(leaves.size());
    for (const auto &leaf : leaves) {
      results.push_back({leaf.identity, detect(leaf.text)});
    }
    return results;
  }
};

} // namespace singularity::protected_api::papinian
