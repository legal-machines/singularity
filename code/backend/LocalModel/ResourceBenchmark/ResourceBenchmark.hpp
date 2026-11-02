#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace singularity::hardware {

// Real OS-reported resource evidence for one model warm-up/load, not
// estimated - model-agnostic, so any registered local model's own
// live_state_provider (see LocalModel/LocalModelRegistry/LocalModelRegistry.hpp) can attach
// one. See docs/LOCAL_INFERENCE.md.
struct ResourceBenchmark {
  double warm_up_ms = 0.0;
  // This process's real peak resident memory at the moment warm-up
  // finished, from Platform/ProcessMetrics/ProcessMetrics.hpp - not a
  // model-only figure (the OS does not expose that), but the honest,
  // measurable proxy this codebase can actually report.
  std::uint64_t peak_resident_memory_bytes = 0;
};

// Times `warm_up` (expected to perform the model's real one-time load, the
// same call path production code takes) and reads the process's real peak
// RSS immediately afterward. Never estimates; if `warm_up` throws, the
// exception propagates and no ResourceBenchmark is produced.
ResourceBenchmark run_resource_benchmark(const std::function<void()> &warm_up);

// What comparing `current` against whatever baseline is stored at
// `baseline_path` under `backend_key` actually showed - see the phase's
// own "compare result to current baseline" requirement. `backend_key`
// should combine the model's own id and version with its chosen
// ExecutionBackend (benchmark_key() below) so unrelated models, versions and
// backends never share one baseline slot.
struct BenchmarkComparison {
  bool has_baseline = false;
  ResourceBenchmark baseline;
  ResourceBenchmark current;
  // True only when `current` is meaningfully worse than `baseline` - see
  // record_and_compare_benchmark()'s tolerance.
  bool regressed = false;
};

// Reads the stored baseline for `backend_key` in `baseline_path` (a small
// local JSON file, never network state) if one exists, compares it against
// `current`, then writes `current` back as the new stored baseline for
// that key. Never throws: a missing/corrupt baseline file is treated as
// "no baseline yet" rather than a benchmark failure.
BenchmarkComparison record_and_compare_benchmark(const std::filesystem::path &baseline_path,
                                                 const std::string &backend_key,
                                                 const ResourceBenchmark &current);

// The slot of one model version on one backend, e.g. "papinian-ner@1.0.0:cpu".
// A different model version never reads, or is compared against, a slot
// another version wrote.
std::string benchmark_key(const std::string &model_id, const std::string &model_version,
                          const std::string &backend);

// What `backend_key` last recorded in `baseline_path`: the previous warm-up
// that finished (a caller records only after its warm-up returned - see
// run_resource_benchmark()). std::nullopt when this model version has never
// finished one here, or when the file is missing or unreadable. Never throws.
std::optional<ResourceBenchmark> recorded_benchmark(const std::filesystem::path &baseline_path,
                                                    const std::string &backend_key);

} // namespace singularity::hardware
