#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace singularity::routing {

enum class ModelLifecycleState { Loading, Ready, Unloaded, Error };

// The real step a running warm-up has reached. The warm-up itself reports
// each step it enters (set_phase below); until it does, and before it has
// begun at all, it is Starting.
enum class ModelWarmupPhase { Starting, LoadingModel, Preparing };

struct ModelLifecycleStatus {
  ModelLifecycleState state = ModelLifecycleState::Loading;
  // Populated only when state == Error.
  std::string message;
  // Meaningful only while state == Loading.
  ModelWarmupPhase phase = ModelWarmupPhase::Starting;
  // Milliseconds since the running warm-up began; 0 before it has begun.
  std::uint64_t elapsed_ms = 0;
};

// A thread-safe, run-exactly-once wrapper around a caller-supplied warm-up
// callable, decoupled from any concrete model so it stays unit-testable
// without the real pinned ONNX/Rust runtime - mirrors the injectable
// Gliner2EngineFactory pattern VerifiedGliner2Runtime already uses for the
// same reason (see LocalModel/Models/PapinianNER/Gliner2Runtime/Gliner2Runtime.hpp).
class ModelWarmupTracker {
public:
  explicit ModelWarmupTracker(std::function<void()> warm_up);

  // Runs warm_up() at most once, even if called concurrently from multiple
  // threads. Catches std::exception and records Error with what() as the
  // message; any other exception records a generic Error message. Never
  // throws - callers only observe outcome through status().
  void run_once();
  // Re-runs a failed warm-up after an external repair (for example after a
  // missing model package has been downloaded and atomically installed).
  void retry();
  // Records an explicit user unload. A later retry() loads the model again.
  void mark_unloaded();
  // Called by the running warm-up as it enters a step. Ignored when no
  // warm-up is running, so a load outside one can never fake progress.
  void set_phase(ModelWarmupPhase phase);

  ModelLifecycleStatus status() const;

private:
  // Marks the first warm-up (run_once) as begun; retry() does the same under
  // the lock that makes it Loading again.
  void begin_warm_up();

  std::function<void()> warm_up_;
  std::once_flag once_;
  bool retry_running_ = false;
  mutable std::mutex mutex_;
  ModelLifecycleStatus status_;
  ModelWarmupPhase phase_ = ModelWarmupPhase::Starting;
  std::optional<std::chrono::steady_clock::time_point> started_;
};

} // namespace singularity::routing
