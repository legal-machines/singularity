#include "Platform/Sha256/Sha256.hpp"
#include "PapinianGuard/Benchmark/PapinianGuardBenchmark.hpp"
#include "LocalModel/Models/PapinianNER/Gliner2Adapter/Gliner2Adapter.hpp"
#include "LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#endif

using nlohmann::json;
using namespace singularity::protected_api;
using namespace singularity::protected_api::papinian;

namespace {

json read_json(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open JSON file: " + path.string());
  return json::parse(input);
}

BenchmarkSuite parse_suite(const std::string &value) {
  if (value == "us-english-legal") return BenchmarkSuite::EnglishLegal;
  if (value == "russian-legal") return BenchmarkSuite::RussianLegal;
  if (value == "mixed-en-ru") return BenchmarkSuite::MixedEnglishRussian;
  throw std::runtime_error("unsupported native benchmark suite: " + value);
}

EntityType parse_type(const std::string &value) {
  if (value == "PERSON") return EntityType::Person;
  if (value == "ORGANISATION") return EntityType::Organisation;
  if (value == "LOCATION") return EntityType::Location;
  throw std::runtime_error("unsupported native benchmark entity type: " + value);
}

std::vector<BenchmarkCase> load_controlled_cases(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open controlled corpus: " + path.string());
  std::vector<BenchmarkCase> cases;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    const auto row = json::parse(line);
    const auto suite_name = row.at("suite").get<std::string>();
    if (suite_name == "generic-privacy-pii") continue;
    BenchmarkCase fixture;
    fixture.id = row.at("document_id").get<std::string>();
    fixture.suite = parse_suite(suite_name);
    fixture.text = row.at("text").get<std::string>();
    for (const auto &gold : row.at("entities")) {
      const auto value = gold.at("text").get<std::string>();
      const auto start = fixture.text.find(value);
      if (start == std::string::npos || fixture.text.find(value, start + 1) != std::string::npos) {
        throw std::runtime_error(fixture.id + ": gold value must occur exactly once");
      }
      fixture.entities.push_back(
          BenchmarkGoldEntity{start, value.size(), parse_type(gold.at("label"))});
    }
    cases.push_back(std::move(fixture));
  }
  if (cases.size() != 12) {
    throw std::runtime_error("controlled native benchmark must contain exactly 12 legal cases");
  }
  return cases;
}

Gliner2ModelPackage load_package(const std::filesystem::path &root,
                                 const std::filesystem::path &provenance_path,
                                 const std::string &package_key) {
  const auto provenance = read_json(provenance_path);
  const auto &probe = provenance.at(package_key);
  std::vector<Gliner2ModelArtifact> artifacts;
  for (auto item = probe.at("artifacts").begin(); item != probe.at("artifacts").end(); ++item) {
    artifacts.push_back({item.key(), item.value().get<std::string>()});
  }
  return Gliner2ModelPackage{
      root,
      probe.at("checkpoint_revision").get<std::string>(),
      probe.at("engine_revision").get<std::string>(),
      probe.at("variant").get<std::string>(),
      std::move(artifacts),
  };
}

std::uint64_t resident_bytes() {
#if defined(__APPLE__)
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
    return info.resident_size;
  }
#endif
  return 0;
}

std::uint64_t peak_resident_bytes() {
#if defined(__APPLE__)
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    return static_cast<std::uint64_t>(usage.ru_maxrss);
  }
#endif
  return 0;
}

std::string sysctl_string(const char *name) {
#if defined(__APPLE__)
  std::size_t size = 0;
  if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) return {};
  std::string value(size, '\0');
  if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) return {};
  while (!value.empty() && value.back() == '\0') value.pop_back();
  return value;
#else
  (void)name;
  return {};
#endif
}

std::string operating_system() {
#if defined(__APPLE__)
  utsname value{};
  if (uname(&value) == 0) {
    return std::string(value.sysname) + " " + value.release + " " + value.machine;
  }
#endif
  return {};
}

std::size_t utf8_byte_to_character_offset(const std::string &text, std::size_t byte_offset) {
  if (byte_offset > text.size()) {
    throw std::runtime_error("native prediction byte offset is outside input");
  }
  std::size_t characters = 0;
  for (std::size_t offset = 0; offset < byte_offset; ++offset) {
    const auto byte = static_cast<unsigned char>(text[offset]);
    if ((byte & 0xc0) != 0x80) ++characters;
  }
  return characters;
}

json prediction_json(const ModelEntity &entity, const std::string &text) {
  return {
      {"start", utf8_byte_to_character_offset(text, entity.start)},
      {"end", utf8_byte_to_character_offset(text, entity.start + entity.length)},
      {"label", entity.label},
      {"confidence", entity.confidence},
  };
}

json metrics_json(const BenchmarkMetrics &metrics) {
  return {
      {"true_positive", metrics.true_positive},
      {"false_positive", metrics.false_positive},
      {"false_negative", metrics.false_negative},
      {"precision", metrics.precision},
      {"recall", metrics.sensitive_entity_recall},
      {"f1", metrics.precision + metrics.sensitive_entity_recall == 0.0
                 ? 0.0
                 : 2.0 * metrics.precision * metrics.sensitive_entity_recall /
                       (metrics.precision + metrics.sensitive_entity_recall)},
      {"mean_latency_ms", metrics.latency_ms},
      {"masking_completeness", metrics.masking_completeness},
      {"restoration_correctness", metrics.restoration_correctness},
  };
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 5 && argc != 6) {
      std::cerr << "usage: papinian_native_benchmark MODEL_DIR CORPUS.jsonl "
                   "DATA_PROVENANCE.json OUTPUT.json [PACKAGE_KEY]\n";
      return 2;
    }
    const std::filesystem::path model_root = argv[1];
    const std::filesystem::path corpus_path = argv[2];
    const std::filesystem::path provenance_path = argv[3];
    const std::filesystem::path output_path = argv[4];
    const std::string package_key = argc == 6 ? argv[5] : "first_native_inference_probe";
    const auto cases = load_controlled_cases(corpus_path);
    const bool force_cpu = std::getenv("FORCE_CPU") != nullptr;
    const auto rss_before = resident_bytes();
    const auto package = load_package(model_root, provenance_path, package_key);
    auto runtime = std::make_shared<VerifiedGliner2Runtime>(
        package);
    auto adapter = std::make_shared<Gliner2SemanticModelAdapter>(runtime);
    PapinianGuard guard(adapter);
    Policy policy;

    const auto cold_started = std::chrono::steady_clock::now();
    const auto cold_result = guard.protect(cases.front().text, policy, "BENCHCOLD0001");
    const auto cold_stopped = std::chrono::steady_clock::now();
    if (!cold_result.validation.masking_complete || !runtime->loaded()) {
      throw std::runtime_error("cold native inference did not complete safely");
    }
    const auto rss_after_load = resident_bytes();

    json raw_predictions = json::array();
    for (const auto &fixture : cases) {
      json entities = json::array();
      for (const auto &entity : adapter->detect(fixture.text)) {
        if (entity.confidence >= 0.5) entities.push_back(prediction_json(entity, fixture.text));
      }
      raw_predictions.push_back({{"document_id", fixture.id}, {"entities", std::move(entities)}});
    }

    std::vector<BenchmarkReport> passes;
    for (int pass = 0; pass < 5; ++pass) {
      passes.push_back(run_benchmark(guard, policy, cases));
    }
    json suites = json::object();
    for (const auto suite : {BenchmarkSuite::EnglishLegal, BenchmarkSuite::RussianLegal,
                             BenchmarkSuite::MixedEnglishRussian}) {
      const auto &quality = passes.front().per_suite.at(suite);
      std::vector<double> warm_means;
      for (const auto &pass : passes) warm_means.push_back(pass.per_suite.at(suite).latency_ms);
      std::sort(warm_means.begin(), warm_means.end());
      auto value = metrics_json(quality);
      value["median_warm_latency_ms"] = warm_means[warm_means.size() / 2];
      suites[to_string(suite)] = std::move(value);
    }

    const auto result = json{
        {"benchmark_version", "papinian-native-controlled-v1"},
        {"runtime", "zero-Python C++ -> Rust -> ONNX Runtime 1.23.2"},
        {"operating_system", operating_system()},
        {"hardware_model", sysctl_string("hw.model")},
        {"processor", sysctl_string("machdep.cpu.brand_string")},
        {"provider_order", force_cpu ? json::array({"CPU"})
                                      : json::array({"CoreML", "CPU"})},
        {"force_cpu", force_cpu},
        {"model_variant", package.variant},
        {"document_count", cases.size()},
        {"corpus_sha256", singularity::platform::sha256_hex_digest_of_file(corpus_path)},
        {"cold_load_and_first_inference_ms",
         std::chrono::duration<double, std::milli>(cold_stopped - cold_started).count()},
        {"rss_before_bytes", rss_before},
        {"rss_after_load_bytes", rss_after_load},
        {"rss_load_delta_bytes", rss_after_load >= rss_before ? rss_after_load - rss_before : 0},
        {"peak_resident_bytes", peak_resident_bytes()},
        {"warm_passes", passes.size()},
        {"same_schema_threshold", 0.5},
        {"requested_labels", Gliner2SemanticModelAdapter::requested_labels()},
        {"raw_predictions", std::move(raw_predictions)},
        {"per_suite", std::move(suites)},
    };
    std::ofstream output(output_path, std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write benchmark output: " + output_path.string());
    output << result.dump(2) << '\n';
    std::cout << result.dump(2) << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "papinian native benchmark failed: " << error.what() << '\n';
    return 1;
  }
}
