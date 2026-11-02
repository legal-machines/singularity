#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace singularity::bridge {

class BackgroundExecutor final {
public:
  explicit BackgroundExecutor(std::size_t thread_count);
  ~BackgroundExecutor();

  BackgroundExecutor(const BackgroundExecutor &) = delete;
  BackgroundExecutor &operator=(const BackgroundExecutor &) = delete;

  void submit(std::function<void()> task);

  // Stops accepting work and returns once every worker has left. Tasks that
  // have not started are dropped: on quit, work nobody is waiting for must
  // never keep the process alive. A task already running finishes; the caller
  // (NativeBridge::shutdown) has already asked live operations to stop.
  void shutdown() noexcept;

private:
  void worker_loop();

  std::condition_variable condition_;
  std::mutex mutex_;
  std::queue<std::function<void()>> tasks_;
  std::vector<std::thread> workers_;
  bool stopping_ = false;
};

} // namespace singularity::bridge
