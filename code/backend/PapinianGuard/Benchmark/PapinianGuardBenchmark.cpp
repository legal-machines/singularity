#include "PapinianGuard/Benchmark/PapinianGuardBenchmark.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <tuple>

namespace singularity::protected_api::papinian {
namespace {

using EntityKey = std::tuple<std::size_t, std::size_t, EntityType>;

EntityType benchmark_type(EntityType type) {
  // The screening corpus intentionally uses the model-neutral PERSON /
  // ORGANISATION / LOCATION groups. Preserve exact spans while folding the
  // product taxonomy's legal refinements into those comparable groups.
  if (type == EntityType::Judge) return EntityType::Person;
  if (type == EntityType::LegalEntity || type == EntityType::Court) {
    return EntityType::Organisation;
  }
  return type;
}

void finalize(BenchmarkMetrics &metrics, std::size_t case_count, std::size_t masked_cases,
              std::size_t restored_cases) {
  const auto expected = metrics.true_positive + metrics.false_negative;
  const auto predicted = metrics.true_positive + metrics.false_positive;
  metrics.sensitive_entity_recall = expected == 0 ? 0.0 : static_cast<double>(metrics.true_positive) / expected;
  metrics.precision = predicted == 0 ? 0.0 : static_cast<double>(metrics.true_positive) / predicted;
  metrics.masking_completeness =
      case_count == 0 ? 0.0 : static_cast<double>(masked_cases) / case_count;
  metrics.restoration_correctness =
      case_count == 0 ? 0.0 : static_cast<double>(restored_cases) / case_count;
}

} // namespace

const char *to_string(BenchmarkSuite suite) {
  switch (suite) {
    case BenchmarkSuite::EnglishLegal:
      return "english_legal";
    case BenchmarkSuite::RussianLegal:
      return "russian_legal";
    case BenchmarkSuite::MixedEnglishRussian:
      return "mixed_en_ru";
  }
  return "english_legal";
}

BenchmarkReport run_benchmark(const PapinianGuard &guard, const Policy &policy,
                              const std::vector<BenchmarkCase> &cases) {
  BenchmarkReport report;
  std::map<BenchmarkSuite, std::size_t> case_counts;
  std::map<BenchmarkSuite, std::size_t> masked_cases;
  std::map<BenchmarkSuite, std::size_t> restored_cases;

  std::size_t request_index = 0;
  for (const auto &fixture : cases) {
    auto &metrics = report.per_suite[fixture.suite];
    case_counts[fixture.suite]++;
    const auto started = std::chrono::steady_clock::now();
    const auto protected_result =
        guard.protect(fixture.text, policy, "BENCH" + std::to_string(10000000 + request_index++));
    const auto stopped = std::chrono::steady_clock::now();
    metrics.latency_ms +=
        std::chrono::duration<double, std::milli>(stopped - started).count();

    std::set<EntityKey> expected;
    for (const auto &entity : fixture.entities) {
      expected.emplace(entity.start, entity.length, benchmark_type(entity.type));
    }
    std::set<EntityKey> predicted;
    for (const auto &entity : protected_result.entities) {
      if (entity.type) {
        predicted.emplace(entity.start, entity.length, benchmark_type(*entity.type));
      }
    }
    for (const auto &entity : expected) {
      if (predicted.find(entity) != predicted.end()) {
        metrics.true_positive++;
      } else {
        metrics.false_negative++;
      }
    }
    for (const auto &entity : predicted) {
      if (expected.find(entity) == expected.end()) metrics.false_positive++;
    }

    bool all_gold_masked = protected_result.validation.masking_complete;
    for (const auto &entity : fixture.entities) {
      const auto raw_value = fixture.text.substr(entity.start, entity.length);
      if (protected_result.protected_text.find(raw_value) != std::string::npos) {
        all_gold_masked = false;
      }
    }
    if (all_gold_masked) masked_cases[fixture.suite]++;

    const auto restored = guard.restore(protected_result.protected_text, protected_result);
    if (restored.integrity_ok && restored.text == fixture.text) restored_cases[fixture.suite]++;
  }

  for (auto &[suite, metrics] : report.per_suite) {
    if (case_counts[suite] != 0) metrics.latency_ms /= case_counts[suite];
    finalize(metrics, case_counts[suite], masked_cases[suite], restored_cases[suite]);
  }
  return report;
}

} // namespace singularity::protected_api::papinian
