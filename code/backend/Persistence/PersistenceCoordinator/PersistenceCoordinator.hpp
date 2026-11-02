#pragma once

#include "Platform/Trace/Trace.hpp"

#include <chrono>
#include <mutex>

namespace singularity::persistence {

// AppSettings and each workspace's Matter registry are persisted as whole-file
// snapshots. Every bridge read-modify-save transaction must hold this lock for
// its complete lifetime so background workers cannot commit stale snapshots.
// Recursive locking is intentional: Matter transactions resolve the active
// workspace through AppSettings while already inside a persistence transaction.
inline std::recursive_mutex &persistence_mutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

// Holds the persistence mutex for a scope, exactly as std::lock_guard does.
// With the diagnostic trace on (Platform/Trace/Trace.hpp) the outermost
// acquisition on a thread also records how long it waited for the lock and how
// long it held it - every reader in the app shares this one lock, so those two
// numbers are where a slow screen usually comes from.
class PersistenceLock final {
public:
  explicit PersistenceLock(std::recursive_mutex &mutex) : mutex_(mutex) {
    if (!platform::trace::enabled()) {
      mutex_.lock();
      return;
    }
    const auto requested = std::chrono::steady_clock::now();
    mutex_.lock();
    if (depth()++ != 0) return;
    outermost_ = true;
    acquired_ = std::chrono::steady_clock::now();
    waited_ms_ = std::chrono::duration<double, std::milli>(acquired_ - requested).count();
  }

  ~PersistenceLock() {
    if (outermost_) {
      const double held_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - acquired_).count();
      --depth();
      // Short, uncontended acquisitions are the normal case and only noise.
      if (waited_ms_ >= 5.0 || held_ms >= 20.0) {
        platform::trace::record("lock", "persistence",
            {{"waitedMs", waited_ms_}, {"heldMs", held_ms}});
      }
    } else if (platform::trace::enabled() && depth() > 0) {
      --depth();
    }
    mutex_.unlock();
  }

  PersistenceLock(const PersistenceLock &) = delete;
  PersistenceLock &operator=(const PersistenceLock &) = delete;

private:
  static int &depth() {
    static thread_local int value = 0;
    return value;
  }

  std::recursive_mutex &mutex_;
  bool outermost_ = false;
  double waited_ms_ = 0.0;
  std::chrono::steady_clock::time_point acquired_;
};

} // namespace singularity::persistence
