#include "LocalModel/ModelLifecycle/ModelLifecycle.hpp"

#include "Platform/Trace/Trace.hpp"

#include <stdexcept>
#include <utility>

namespace singularity::routing {

ModelWarmupTracker::ModelWarmupTracker(std::function<void()> warm_up)
    : warm_up_(std::move(warm_up)) {}

void ModelWarmupTracker::run_once() {
  std::call_once(once_, [this] {
    begin_warm_up();
    try {
      warm_up_();
      platform::trace::record("model", "warm_up_ready");
      std::lock_guard<std::mutex> lock(mutex_);
      status_ = ModelLifecycleStatus{ModelLifecycleState::Ready, ""};
    } catch (const std::exception &error) {
      // Why a model would not start, in the developer's trace when it is on
      // (SINGULARITY_TRACE_FILE): a Release build's protection that did not
      // start said only that it had not. Never a document's words: a warm-up
      // reads none.
      platform::trace::record("model", "warm_up_failed", {{"reason", std::string(error.what())}});
      std::lock_guard<std::mutex> lock(mutex_);
      status_ = ModelLifecycleStatus{ModelLifecycleState::Error, error.what()};
    } catch (...) {
      platform::trace::record("model", "warm_up_failed", {{"reason", std::string("unknown")}});
      std::lock_guard<std::mutex> lock(mutex_);
      status_ = ModelLifecycleStatus{ModelLifecycleState::Error,
                                     "Unknown error during model warm-up"};
    }
  });
}

ModelLifecycleStatus ModelWarmupTracker::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  ModelLifecycleStatus result = status_;
  if (result.state == ModelLifecycleState::Loading) {
    result.phase = phase_;
    if (started_) {
      result.elapsed_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - *started_).count());
    }
  }
  return result;
}

void ModelWarmupTracker::retry() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if ((status_.state != ModelLifecycleState::Error &&
         status_.state != ModelLifecycleState::Unloaded) || retry_running_) return;
    retry_running_ = true;
    status_ = ModelLifecycleStatus{ModelLifecycleState::Loading, ""};
    started_ = std::chrono::steady_clock::now();
    phase_ = ModelWarmupPhase::Starting;
  }
  try {
    warm_up_();
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = ModelLifecycleStatus{ModelLifecycleState::Ready, ""};
    retry_running_ = false;
  } catch (const std::exception &error) {
    platform::trace::record("model", "warm_up_failed", {{"reason", std::string(error.what())}, {"retry", true}});
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = ModelLifecycleStatus{ModelLifecycleState::Error, error.what()};
    retry_running_ = false;
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = ModelLifecycleStatus{ModelLifecycleState::Error, "Unknown error during model warm-up"};
    retry_running_ = false;
  }
}

void ModelWarmupTracker::mark_unloaded() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (retry_running_ || status_.state == ModelLifecycleState::Loading) {
    throw std::runtime_error("The model is still loading. Wait for it to finish before unloading it.");
  }
  status_ = ModelLifecycleStatus{ModelLifecycleState::Unloaded, ""};
}

void ModelWarmupTracker::set_phase(ModelWarmupPhase phase) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (status_.state != ModelLifecycleState::Loading || !started_) return;
  phase_ = phase;
}

void ModelWarmupTracker::begin_warm_up() {
  std::lock_guard<std::mutex> lock(mutex_);
  started_ = std::chrono::steady_clock::now();
  phase_ = ModelWarmupPhase::Starting;
}

} // namespace singularity::routing
