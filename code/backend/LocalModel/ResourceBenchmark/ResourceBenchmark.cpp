#include "LocalModel/ResourceBenchmark/ResourceBenchmark.hpp"

#include "Platform/ProcessMetrics/ProcessMetrics.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <atomic>
#include <thread>
#include <fstream>
#include <system_error>

namespace singularity::hardware {

ResourceBenchmark run_resource_benchmark(const std::function<void()> &warm_up) {
  const auto started = std::chrono::steady_clock::now();
  warm_up();
  const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

  ResourceBenchmark result;
  result.warm_up_ms = elapsed;
  result.peak_resident_memory_bytes = platform::peak_resident_memory_bytes();
  return result;
}

namespace {

using Json = nlohmann::json;

Json resource_benchmark_json(const ResourceBenchmark &benchmark) {
  return Json{{"warmUpMs", benchmark.warm_up_ms},
             {"peakResidentMemoryBytes", benchmark.peak_resident_memory_bytes}};
}

ResourceBenchmark resource_benchmark_from_json(const Json &value) {
  ResourceBenchmark result;
  result.warm_up_ms = value.value("warmUpMs", 0.0);
  result.peak_resident_memory_bytes = value.value("peakResidentMemoryBytes", std::uint64_t{0});
  return result;
}

// Timing is inherently noisy across separate process launches (OS
// scheduling, thermal state, background load); memory is comparatively
// stable. A regression must be meaningfully outside normal run-to-run
// variance, not a single-digit-percent wobble, before it is worth flagging.
constexpr double kWarmUpRegressionTolerance = 1.5;
constexpr double kMemoryRegressionTolerance = 1.15;

// A missing or corrupt file is simply an empty store.
Json read_store(const std::filesystem::path &baseline_path) {
  Json store = Json::object();
  try {
    std::ifstream input(baseline_path);
    if (input) input >> store;
    if (!store.is_object()) store = Json::object();
  } catch (...) {
    store = Json::object();
  }
  return store;
}

} // namespace

BenchmarkComparison record_and_compare_benchmark(const std::filesystem::path &baseline_path,
                                                 const std::string &backend_key,
                                                 const ResourceBenchmark &current) {
  BenchmarkComparison comparison;
  comparison.current = current;

  Json store = read_store(baseline_path);

  if (store.contains(backend_key)) {
    comparison.has_baseline = true;
    comparison.baseline = resource_benchmark_from_json(store.at(backend_key));
    comparison.regressed =
        (comparison.baseline.warm_up_ms > 0.0 &&
         current.warm_up_ms > comparison.baseline.warm_up_ms * kWarmUpRegressionTolerance) ||
        (comparison.baseline.peak_resident_memory_bytes > 0 &&
         current.peak_resident_memory_bytes >
             static_cast<std::uint64_t>(comparison.baseline.peak_resident_memory_bytes *
                                        kMemoryRegressionTolerance));
  }

  store[backend_key] = resource_benchmark_json(current);

  try {
    std::error_code error;
    std::filesystem::create_directories(baseline_path.parent_path(), error);
    std::ofstream output(baseline_path, std::ios::trunc);
    if (output) output << store.dump(2);
  } catch (...) {
    // Persisting the new baseline is best-effort; the comparison already
    // computed above remains valid either way.
  }

  return comparison;
}

std::string benchmark_key(const std::string &model_id, const std::string &model_version,
                          const std::string &backend) {
  return model_id + "@" + model_version + ":" + backend;
}

std::optional<ResourceBenchmark> recorded_benchmark(const std::filesystem::path &baseline_path,
                                                    const std::string &backend_key) {
  const Json store = read_store(baseline_path);
  const auto entry = store.find(backend_key);
  if (entry == store.end() || !entry->is_object()) return std::nullopt;
  const auto warm_up = entry->find("warmUpMs");
  if (warm_up == entry->end() || !warm_up->is_number() || warm_up->get<double>() <= 0.0) return std::nullopt;

  ResourceBenchmark result;
  result.warm_up_ms = warm_up->get<double>();
  const auto memory = entry->find("peakResidentMemoryBytes");
  if (memory != entry->end() && memory->is_number_unsigned()) {
    result.peak_resident_memory_bytes = memory->get<std::uint64_t>();
  }
  return result;
}

} // namespace singularity::hardware
