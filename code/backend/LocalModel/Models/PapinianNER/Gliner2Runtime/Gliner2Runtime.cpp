#include "LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.hpp"

#include "Platform/Sha256/Sha256.hpp"
#include "Routing/Observability/OperationEvent.hpp"
#include "LocalModel/Models/PapinianNER/NativeRuntime/gliner2_ffi.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace singularity::protected_api::papinian {
namespace {

bool valid_sha256(const std::string &value) {
  if (value.size() != 64) return false;
  for (const auto byte : value) {
    if (!std::isdigit(static_cast<unsigned char>(byte)) &&
        !(byte >= 'a' && byte <= 'f')) {
      return false;
    }
  }
  return true;
}

bool confined_relative_path(const std::filesystem::path &path) {
  if (path.empty() || path.is_absolute()) return false;
  for (const auto &part : path) {
    if (part == "..") return false;
  }
  return true;
}

const std::array<std::string, 8> &required_v2_roles() {
  static const std::array<std::string, 8> roles{
      "encoder", "token_gather", "span_rep", "schema_gather",
      "count_pred_argmax", "count_lstm_fixed", "scorer", "classifier",
  };
  return roles;
}

std::filesystem::path canonical_existing_path(const std::filesystem::path &path,
                                               const std::string &description) {
  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  if (error) {
    throw std::runtime_error(description + ": " + path.string());
  }
  return canonical;
}

bool path_is_within(const std::filesystem::path &root, const std::filesystem::path &candidate) {
  auto root_part = root.begin();
  auto candidate_part = candidate.begin();
  for (; root_part != root.end(); ++root_part, ++candidate_part) {
    if (candidate_part == candidate.end() || *candidate_part != *root_part) return false;
  }
  return true;
}

class NativeGliner2InferenceEngine final : public IGliner2InferenceEngine {
public:
  explicit NativeGliner2InferenceEngine(const Gliner2ModelPackage &package) {
    char *error = nullptr;
    engine_ = lm_gliner2_engine_create(package.root.string().c_str(), &error);
    if (!engine_) {
      const std::string message = error ? error : "unknown native runtime error";
      lm_gliner2_string_free(error);
      throw std::runtime_error(message);
    }
  }

  ~NativeGliner2InferenceEngine() override { lm_gliner2_engine_destroy(engine_); }

  std::vector<Gliner2RuntimeEntity>
  infer(const std::string &utf8_text, const std::vector<std::string> &labels) const override {
    std::vector<const char *> label_pointers;
    label_pointers.reserve(labels.size());
    for (const auto &label : labels) label_pointers.push_back(label.c_str());

    LmGliner2EntityList native_result{};
    char *error = nullptr;
    if (!lm_gliner2_engine_infer_with_progress(engine_, utf8_text.c_str(), label_pointers.data(),
                                 label_pointers.size(), &native_result, &error,
                                 [](std::size_t completed, std::size_t total, void *) {
                                   try {
                                     namespace obs = singularity::routing::observability;
                                     obs::emit({obs::OperationEventKind::PapinianProgress, "Semantic chunks completed",
                                         {{"completed", std::to_string(completed)}, {"total", std::to_string(total)}}, std::nullopt});
                                   } catch (...) {} // Observation must not unwind across the C ABI.
                                 }, nullptr)) {
      const std::string message = error ? error : "unknown native inference error";
      lm_gliner2_string_free(error);
      throw std::runtime_error(message);
    }

    struct ResultOwner {
      LmGliner2EntityList value;
      ~ResultOwner() { lm_gliner2_entity_list_free(value); }
    } owner{native_result};

    std::vector<std::size_t> requested_byte_offsets;
    requested_byte_offsets.reserve(native_result.len * 2);
    for (std::size_t index = 0; index < native_result.len; ++index) {
      const auto &entity = native_result.entities[index];
      if (!entity.label || entity.end_byte < entity.start_byte) {
        throw std::runtime_error("GLiNER2 native engine returned an invalid entity");
      }
      requested_byte_offsets.push_back(entity.start_byte);
      requested_byte_offsets.push_back(entity.end_byte);
    }
    namespace obs = singularity::routing::observability;
    obs::emit({obs::OperationEventKind::PapinianOffsetResolution, "Resolving UTF-8 entity offsets",
        {{"processed", "0"}, {"total", std::to_string(requested_byte_offsets.size())}}, std::nullopt});
    const auto scalar_offsets = utf8_scalar_offsets(utf8_text, requested_byte_offsets);
    obs::emit({obs::OperationEventKind::PapinianOffsetResolution, "Resolved UTF-8 entity offsets",
        {{"processed", std::to_string(requested_byte_offsets.size())},
         {"total", std::to_string(requested_byte_offsets.size())}}, std::nullopt});

    std::vector<Gliner2RuntimeEntity> result;
    result.reserve(native_result.len);
    for (std::size_t index = 0; index < native_result.len; ++index) {
      const auto &entity = native_result.entities[index];
      const auto scalar_start = scalar_offsets.at(entity.start_byte);
      const auto scalar_end = scalar_offsets.at(entity.end_byte);
      result.push_back(Gliner2RuntimeEntity{scalar_start, scalar_end - scalar_start,
                                            entity.label, entity.confidence});
    }
    return result;
  }

  std::vector<std::vector<Gliner2RuntimeEntity>>
  infer_batch(const std::vector<std::string> &utf8_texts,
              const std::vector<std::string> &labels,
              std::size_t microbatch_size) const override {
    std::vector<const char *> text_pointers;
    text_pointers.reserve(utf8_texts.size());
    for (const auto &text : utf8_texts) text_pointers.push_back(text.c_str());
    std::vector<const char *> label_pointers;
    label_pointers.reserve(labels.size());
    for (const auto &label : labels) label_pointers.push_back(label.c_str());

    LmGliner2EntityBatch native_result{};
    char *error = nullptr;
    if (!lm_gliner2_engine_infer_batch_with_progress(
            engine_, text_pointers.data(), text_pointers.size(), label_pointers.data(),
            label_pointers.size(), microbatch_size, &native_result, &error,
            [](std::size_t completed, std::size_t total, void *) {
              try {
                namespace obs = singularity::routing::observability;
                obs::emit({obs::OperationEventKind::PapinianProgress,
                    "Semantic batch chunks completed",
                    {{"completed", std::to_string(completed)}, {"total", std::to_string(total)}},
                    std::nullopt});
              } catch (...) {}
            }, nullptr)) {
      const std::string message = error ? error : "unknown native batch inference error";
      lm_gliner2_string_free(error);
      throw std::runtime_error(message);
    }
    struct BatchOwner {
      LmGliner2EntityBatch value;
      ~BatchOwner() { lm_gliner2_entity_batch_free(value); }
    } owner{native_result};
    if (native_result.len != utf8_texts.size()) {
      throw std::runtime_error("GLiNER2 native batch returned the wrong sample count");
    }

    std::vector<std::vector<Gliner2RuntimeEntity>> results(native_result.len);
    for (std::size_t sample = 0; sample < native_result.len; ++sample) {
      const auto &native_entities = native_result.samples[sample];
      std::vector<std::size_t> requested_offsets;
      requested_offsets.reserve(native_entities.len * 2);
      for (std::size_t index = 0; index < native_entities.len; ++index) {
        const auto &entity = native_entities.entities[index];
        if (!entity.label || entity.end_byte < entity.start_byte) {
          throw std::runtime_error("GLiNER2 native batch returned an invalid entity");
        }
        requested_offsets.push_back(entity.start_byte);
        requested_offsets.push_back(entity.end_byte);
      }
      const auto scalar_offsets = utf8_scalar_offsets(utf8_texts[sample], requested_offsets);
      auto &result = results[sample];
      result.reserve(native_entities.len);
      for (std::size_t index = 0; index < native_entities.len; ++index) {
        const auto &entity = native_entities.entities[index];
        const auto start = scalar_offsets.at(entity.start_byte);
        const auto end = scalar_offsets.at(entity.end_byte);
        result.push_back({start, end - start, entity.label, entity.confidence});
      }
    }
    return results;
  }

private:
  void *engine_ = nullptr;
};

} // namespace

std::shared_ptr<const IGliner2InferenceEngine>
make_native_gliner2_engine(const Gliner2ModelPackage &package) {
  return std::make_shared<NativeGliner2InferenceEngine>(package);
}

void verify_gliner2_model_package(const Gliner2ModelPackage &package) {
  if (package.root.empty() || package.checkpoint_revision.empty() ||
      package.export_revision.empty() || package.variant.empty()) {
    throw std::runtime_error("GLiNER2 package identity is incomplete");
  }
  if (package.artifacts.empty()) {
    throw std::runtime_error("GLiNER2 package contains no artifacts");
  }
  if (package.artifacts.size() != required_v2_roles().size() + 1) {
    throw std::runtime_error(
        "GLiNER2 V2 package must pin exactly eight ONNX fragments and tokenizer.json");
  }

  const auto canonical_root =
      canonical_existing_path(package.root, "GLiNER2 package root is missing");
  std::error_code root_error;
  if (!std::filesystem::is_directory(canonical_root, root_error) || root_error) {
    throw std::runtime_error("GLiNER2 package root is not a directory: " + package.root.string());
  }

  std::set<std::filesystem::path> seen;
  std::map<std::string, std::filesystem::path> role_paths;
  std::filesystem::path tokenizer_path;
  std::string onnx_suffix;
  for (const auto &artifact : package.artifacts) {
    if (!confined_relative_path(artifact.relative_path)) {
      throw std::runtime_error("GLiNER2 artifact path escapes its package root");
    }
    if (artifact.relative_path != artifact.relative_path.filename()) {
      throw std::runtime_error("GLiNER2 runtime artifacts must be direct package-root files");
    }
    if (!seen.insert(artifact.relative_path.lexically_normal()).second) {
      throw std::runtime_error("GLiNER2 package contains a duplicate artifact path");
    }
    if (!valid_sha256(artifact.sha256)) {
      throw std::runtime_error("GLiNER2 artifact has an invalid SHA-256");
    }

    const auto path = package.root / artifact.relative_path;
    std::error_code error;
    if (std::filesystem::is_symlink(path, error) || error) {
      throw std::runtime_error("GLiNER2 artifact must not be a symbolic link: " + path.string());
    }
    if (!std::filesystem::is_regular_file(path, error) || error) {
      throw std::runtime_error("GLiNER2 artifact is missing: " + path.string());
    }
    const auto canonical_path = canonical_existing_path(path, "GLiNER2 artifact is missing");
    if (!path_is_within(canonical_root, canonical_path)) {
      throw std::runtime_error("GLiNER2 artifact resolves outside its package root: " +
                               path.string());
    }

    const auto filename = artifact.relative_path.filename().string();
    if (filename == "tokenizer.json") {
      if (!tokenizer_path.empty()) {
        throw std::runtime_error("GLiNER2 V2 package contains multiple tokenizer.json files");
      }
      tokenizer_path = artifact.relative_path;
    } else {
      const auto role =
          std::find_if(required_v2_roles().begin(), required_v2_roles().end(),
                       [&](const auto &candidate) {
                         return filename.rfind(candidate + "_", 0) == 0;
                       });
      if (role == required_v2_roles().end() || artifact.relative_path.extension() != ".onnx") {
        throw std::runtime_error("GLiNER2 package contains an unknown runtime artifact: " +
                                 artifact.relative_path.string());
      }
      const auto suffix = filename.substr(role->size());
      if (onnx_suffix.empty()) {
        onnx_suffix = suffix;
      } else if (suffix != onnx_suffix) {
        throw std::runtime_error("GLiNER2 package mixes ONNX fragment variants");
      }
      if (!role_paths.emplace(*role, artifact.relative_path).second) {
        throw std::runtime_error("GLiNER2 V2 package contains duplicate ONNX role: " + *role);
      }
    }

    const auto actual = platform::sha256_hex_digest_of_file(path);
    if (actual != artifact.sha256) {
      throw std::runtime_error("GLiNER2 artifact hash mismatch: " + path.string());
    }
  }

  for (const auto &entry : std::filesystem::directory_iterator(canonical_root)) {
    std::error_code error;
    if (entry.is_symlink(error) || error || !entry.is_regular_file(error) || error) {
      throw std::runtime_error("GLiNER2 package contains a non-artifact entry: " +
                               entry.path().string());
    }
    const auto relative_path = entry.path().filename();
    if (seen.find(relative_path) == seen.end()) {
      throw std::runtime_error("GLiNER2 package contains an unpinned artifact: " +
                               entry.path().string());
    }
  }

  if (tokenizer_path.empty()) {
    throw std::runtime_error("GLiNER2 V2 package is missing tokenizer.json");
  }
  for (const auto &role : required_v2_roles()) {
    if (role_paths.find(role) == role_paths.end()) {
      throw std::runtime_error("GLiNER2 V2 package is missing ONNX role: " + role);
    }
  }
}

VerifiedGliner2Runtime::VerifiedGliner2Runtime(Gliner2ModelPackage package,
                                               Gliner2EngineFactory factory)
    : package_(std::move(package)), factory_(std::move(factory)) {
  if (!factory_) throw std::invalid_argument("GLiNER2 engine factory is required");
}

VerifiedGliner2Runtime::VerifiedGliner2Runtime(Gliner2ModelPackage package)
    : VerifiedGliner2Runtime(std::move(package), make_native_gliner2_engine) {}

std::shared_ptr<const IGliner2InferenceEngine> VerifiedGliner2Runtime::engine() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (engine_) return engine_;
  if (!failure_.empty()) throw std::runtime_error(failure_);

  try {
    verify_gliner2_model_package(package_);
    auto loaded = factory_(package_);
    if (!loaded) throw std::runtime_error("GLiNER2 engine factory returned null");
    // The engine contract requires eager opens, but a factory is still an
    // injection boundary. Recheck after construction so replacement during a
    // slow multi-session load cannot turn a verified package into an accepted
    // unverified engine.
    verify_gliner2_model_package(package_);
    engine_ = std::move(loaded);
    return engine_;
  } catch (const std::exception &error) {
    failure_ = std::string("GLiNER2 runtime unavailable: ") + error.what();
    throw std::runtime_error(failure_);
  }
}

std::vector<Gliner2RuntimeEntity>
VerifiedGliner2Runtime::infer(const std::string &utf8_text,
                             const std::vector<std::string> &labels) const {
  return engine()->infer(utf8_text, labels);
}

std::vector<std::vector<Gliner2RuntimeEntity>>
VerifiedGliner2Runtime::infer_batch(const std::vector<std::string> &utf8_texts,
                                    const std::vector<std::string> &labels,
                                    std::size_t microbatch_size) const {
  return engine()->infer_batch(utf8_texts, labels, microbatch_size);
}

bool VerifiedGliner2Runtime::loaded() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<bool>(engine_);
}

void VerifiedGliner2Runtime::unload() {
  std::shared_ptr<const IGliner2InferenceEngine> released;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    released = std::move(engine_);
    failure_.clear();
  }
  // Destroy outside the runtime mutex; native teardown may be expensive and
  // an in-flight request can still own the previous shared engine safely.
  released.reset();
}

Gliner2ModelPackage pinned_fastino_v2_model_package(const std::filesystem::path &root) {
  return Gliner2ModelPackage{
      root,
      "c153999da5f4c509df4322b0c6a1baf3d2c284d7",
      "62376385092826094a1810d81050440639fec7ac",
      "fp16_v2-macos-standard-io",
      {
          {"encoder_fp16.onnx", "60f5e1b343a53cb579e7e2758b618f20d58ff1b57d5a66ba1ac7798d8e935ccd"},
          {"token_gather_fp16.onnx", "7e71eaccdc645a848968302c2127840cf7c23383389d77390acd68e232bbb325"},
          {"span_rep_fp16.onnx", "f5b73ddd986cb20e5b1862be313cebec9c5a9d045f7e1e8fc1c4484e889786f5"},
          {"schema_gather_fp16.onnx", "020f5d0a4f17ee3b3f45784ed759fd3d266855692eb1dec7ab1324b6600e0074"},
          {"count_pred_argmax_fp16.onnx", "66e9fed4cdb678e85798dbe3b8d51e98ccf3f68f3c68894d4a0c557ffe5a70fa"},
          {"count_lstm_fixed_fp16.onnx", "5d45c49ca07a6c90aa36a1d2b5e5f2ffd11b9c3302fae24adcfd29841668b9ad"},
          {"scorer_fp16.onnx", "51acf0dcd506ec6b9cfa69ed0c6051c98b663594914948e0171af635b76d68d4"},
          {"classifier_fp16.onnx", "d535c71fe0dced52324c429fd1bab9d831fec0fa7f9b7149e54f7a9d15259db0"},
          {"tokenizer.json", "f6df10ec83bea993035b2dd7c39345a3d4fcf23421c2adb6cb4ffc1e6d1bc4b5"},
      },
  };
}

std::size_t gliner2_encoder_forward_calls_total() {
  return lm_gliner2_encoder_forward_calls_total();
}

std::size_t gliner2_encoder_batches_total() {
  return lm_gliner2_encoder_batches_total();
}

std::size_t gliner2_downstream_forward_calls_total() {
  return lm_gliner2_downstream_forward_calls_total();
}

} // namespace singularity::protected_api::papinian
