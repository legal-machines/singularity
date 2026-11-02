#include "LocalModel/Models/PapinianNER/Gliner2Adapter/Gliner2Adapter.hpp"

#include <stdexcept>
#include <utility>

namespace singularity::protected_api::papinian {
namespace {

std::vector<std::size_t> utf8_character_boundaries(const std::string &text) {
  std::vector<std::size_t> boundaries;
  boundaries.reserve(text.size() + 1);
  for (std::size_t offset = 0; offset < text.size();) {
    boundaries.push_back(offset);
    const auto lead = static_cast<unsigned char>(text[offset]);
    std::size_t width = 0;
    if (lead <= 0x7f) width = 1;
    else if (lead >= 0xc2 && lead <= 0xdf) width = 2;
    else if (lead >= 0xe0 && lead <= 0xef) width = 3;
    else if (lead >= 0xf0 && lead <= 0xf4) width = 4;
    else throw std::runtime_error("GLiNER2 input is not valid UTF-8");

    if (offset + width > text.size()) {
      throw std::runtime_error("GLiNER2 input is not valid UTF-8");
    }
    for (std::size_t index = 1; index < width; ++index) {
      const auto continuation = static_cast<unsigned char>(text[offset + index]);
      if ((continuation & 0xc0) != 0x80) {
        throw std::runtime_error("GLiNER2 input is not valid UTF-8");
      }
    }
    if ((width == 3 && lead == 0xe0 && static_cast<unsigned char>(text[offset + 1]) < 0xa0) ||
        (width == 3 && lead == 0xed && static_cast<unsigned char>(text[offset + 1]) >= 0xa0) ||
        (width == 4 && lead == 0xf0 && static_cast<unsigned char>(text[offset + 1]) < 0x90) ||
        (width == 4 && lead == 0xf4 && static_cast<unsigned char>(text[offset + 1]) >= 0x90)) {
      throw std::runtime_error("GLiNER2 input is not valid UTF-8");
    }
    offset += width;
  }
  boundaries.push_back(text.size());
  return boundaries;
}

} // namespace

Gliner2SemanticModelAdapter::Gliner2SemanticModelAdapter(
    std::shared_ptr<const IGliner2Runtime> runtime)
    : runtime_(std::move(runtime)) {
  if (!runtime_) throw std::invalid_argument("GLiNER2 runtime is required");
}

SemanticModelIdentity Gliner2SemanticModelAdapter::identity() const {
  return SemanticModelIdentity{
      "Papinian semantic model adapter (GLiNER2)",
      "fastino/gliner2-privacy-filter-PII-multi",
      "c153999da5f4c509df4322b0c6a1baf3d2c284d7",
      "Third-party Fastino GLiNER2 PII checkpoint (Apache-2.0 declared), using "
      "microsoft/mdeberta-v3-base (MIT); unchanged weights are not Papinian NER",
      false,
  };
}

const std::vector<std::string> &Gliner2SemanticModelAdapter::requested_labels() {
  static const std::vector<std::string> labels{
      "person",          "legal entity",       "organization", "location", "address",
      "email",           "phone number",       "bank account", "tax id",
      "case number",     "contract identifier", "court",        "judge",
      "date",            "money amount",
  };
  return labels;
}

std::vector<ModelEntity> Gliner2SemanticModelAdapter::detect(const std::string &utf8_text) const {
  std::vector<ModelEntity> result;
  const auto boundaries = utf8_character_boundaries(utf8_text);
  for (const auto &entity : runtime_->infer(utf8_text, requested_labels())) {
    if (entity.start >= boundaries.size() || entity.length > boundaries.size() - entity.start - 1) {
      continue;
    }
    const auto byte_start = boundaries[entity.start];
    const auto byte_end = boundaries[entity.start + entity.length];
    result.push_back(
        ModelEntity{byte_start, byte_end - byte_start, entity.label, entity.confidence});
  }
  return result;
}

std::vector<SemanticLeafDetection>
Gliner2SemanticModelAdapter::detect_batch(const std::vector<SemanticLeaf> &leaves) const {
  std::vector<std::vector<std::size_t>> boundaries;
  std::vector<std::string> texts;
  boundaries.reserve(leaves.size());
  texts.reserve(leaves.size());
  for (const auto &leaf : leaves) {
    boundaries.push_back(utf8_character_boundaries(leaf.text));
    texts.push_back(leaf.text);
  }
  const auto inferred = runtime_->infer_batch(texts, requested_labels());
  if (inferred.size() != leaves.size()) {
    throw std::runtime_error("GLiNER2 batch result count does not match semantic leaf count");
  }
  std::vector<SemanticLeafDetection> results;
  results.reserve(leaves.size());
  for (std::size_t index = 0; index < leaves.size(); ++index) {
    SemanticLeafDetection result;
    result.identity = leaves[index].identity;
    for (const auto &entity : inferred[index]) {
      if (entity.start >= boundaries[index].size()
          || entity.length > boundaries[index].size() - entity.start - 1) continue;
      const auto byte_start = boundaries[index][entity.start];
      const auto byte_end = boundaries[index][entity.start + entity.length];
      result.entities.push_back(
          ModelEntity{byte_start, byte_end - byte_start, entity.label, entity.confidence});
    }
    results.push_back(std::move(result));
  }
  return results;
}

} // namespace singularity::protected_api::papinian
