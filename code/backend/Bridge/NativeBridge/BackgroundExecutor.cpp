#include "Bridge/NativeBridge/BackgroundExecutor.hpp"

#include <stdexcept>
#include <utility>

namespace singularity::bridge {

BackgroundExecutor::BackgroundExecutor(std::size_t thread_count) {
  if (thread_count == 0) {
    throw std::invalid_argument("BackgroundExecutor requires at least one thread");
  }
  workers_.reserve(thread_count);
  for (std::size_t index = 0; index < thread_count; ++index) {
    workers_.emplace_back([this] { worker_loop(); });
  }
}

BackgroundExecutor::~BackgroundExecutor() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  condition_.notify_all();
  for (auto &worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

void BackgroundExecutor::shutdown() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    std::queue<std::function<void()>>().swap(tasks_);
  }
  condition_.notify_all();
  for (auto &worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
}

void BackgroundExecutor::submit(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      throw std::runtime_error("BackgroundExecutor is stopping");
    }
    tasks_.push(std::move(task));
  }
  condition_.notify_one();
}

void BackgroundExecutor::worker_loop() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      if (stopping_ && tasks_.empty()) {
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop();
    }
    task();
  }
}

} // namespace singularity::bridge
