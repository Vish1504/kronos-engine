#pragma once

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

namespace kronos {

template <typename T> class thread_safe_queue {
public:
  bool push(T task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);

      if (shutting_down_) {
        return false;
      }

      queue_.push(std::move(task));
    }

    cv_.notify_one();

    return true;
  }

  std::optional<T> wait_and_pop() {
    std::unique_lock<std::mutex> lock(mutex_);

    cv_.wait(lock, [this] { return !queue_.empty() || shutting_down_; });
    // if shutdown
    if (shutting_down_ == true && queue_.empty()) {
      return std::nullopt;
    }

    T task = std::move(queue_.front());
    queue_.pop();
    return task;
  }

  void shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    {

      // Ignore if already shutting down
      if (shutting_down_) {
        return;
      }
      shutting_down_ = true;
      /* Wake up all sleeping threads so they can notice the shutdown flag and
       exit cleanly.*/
      cv_.notify_all();
    }
  }

private:
  std::queue<T> queue_;

  std::mutex mutex_;

  std::condition_variable cv_;

  bool shutting_down_ = false;
};

} // namespace kronos