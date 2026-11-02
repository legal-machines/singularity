#pragma once

#include "PapinianGuard/PapinianGuardService/PapinianGuardService.hpp"

#include <map>

namespace singularity::protected_api::papinian {

enum class BenchmarkSuite {
  EnglishLegal,
  RussianLegal,
  MixedEnglishRussian,
};

const char *to_string(BenchmarkSuite suite);

struct BenchmarkGoldEntity {
  std::size_t start = 0;
  std::size_t length = 0;
  EntityType type = EntityType::Person;
};

struct BenchmarkCase {
  std::string id;
  BenchmarkSuite suite = BenchmarkSuite::EnglishLegal;
  std::string text;
  std::vector<BenchmarkGoldEntity> entities;
};

struct BenchmarkMetrics {
  std::size_t true_positive = 0;
  std::size_t false_positive = 0;
  std::size_t false_negative = 0;
  double sensitive_entity_recall = 0.0;
  double precision = 0.0;
  double latency_ms = 0.0;
  double masking_completeness = 0.0;
  double restoration_correctness = 0.0;
};

struct BenchmarkReport {
  std::map<BenchmarkSuite, BenchmarkMetrics> per_suite;
};

// Runs only caller-supplied controlled fixtures. The harness never discovers
// or reads Matter data. Runtime/model latency is included when the supplied
// PapinianGuard owns a real adapter; replay adapters measure integration only.
BenchmarkReport run_benchmark(const PapinianGuard &guard, const Policy &policy,
                              const std::vector<BenchmarkCase> &cases);

} // namespace singularity::protected_api::papinian
