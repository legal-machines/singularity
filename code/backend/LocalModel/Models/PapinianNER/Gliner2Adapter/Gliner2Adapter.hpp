#pragma once

#include "LocalModel/Models/PapinianNER/SemanticModel/SemanticModel.hpp"

#include <memory>

namespace singularity::protected_api::papinian {

struct Gliner2RuntimeEntity {
  // GLiNER2/tokenizer offsets are Unicode-scalar positions. The adapter
  // converts them to UTF-8 byte offsets before exposing ModelEntity.
  std::size_t start = 0;
  std::size_t length = 0;
  std::string label;
  double confidence = 0.0;
};

// The only GLiNER2-specific runtime contract. A future ONNX implementation
// belongs behind this interface; Papinian Guard and the rest of Singularity never
// include or depend on GLiNER2 classes.
class IGliner2Runtime {
public:
  virtual ~IGliner2Runtime() = default;
  // Returned start/length values are Unicode-scalar offsets, never UTF-8
  // byte offsets. This keeps tokenizer-specific indexing inside this seam.
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

class Gliner2SemanticModelAdapter final : public ISemanticModelAdapter {
public:
  explicit Gliner2SemanticModelAdapter(std::shared_ptr<const IGliner2Runtime> runtime);

  SemanticModelIdentity identity() const override;
  std::vector<ModelEntity> detect(const std::string &utf8_text) const override;
  std::vector<SemanticLeafDetection>
  detect_batch(const std::vector<SemanticLeaf> &leaves) const override;

  static const std::vector<std::string> &requested_labels();

private:
  std::shared_ptr<const IGliner2Runtime> runtime_;
};

} // namespace singularity::protected_api::papinian
