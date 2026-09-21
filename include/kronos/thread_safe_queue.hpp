// #pragma once

// #include <condition_variable>
// #include <iostream>
// #include <mutex>
// #include <optional>
// #include <queue>
// #include <utility>

// namespace kronos {

// template <typename T> class thread_safe_queue {
// public:
//   bool push(T task) {
//     {
//       std::lock_guard<std::mutex> lock(mutex_);

//       if (shutting_down_) {
//         return false;
//       }

//       queue_.push(std::move(task));
//     }

//     cv_.notify_one();

//     return true;
//   }

//   std::optional<T> wait_and_pop() {
//     std::unique_lock<std::mutex> lock(mutex_);

//     cv_.wait(lock, [this] { return !queue_.empty() || shutting_down_; });
//     // if shutdown
//     if (shutting_down_ == true && queue_.empty()) {
//       return std::nullopt;
//     }

//     T task = std::move(queue_.front());
//     queue_.pop();
//     return task;
//   }

//   void shutdown() {
//     std::lock_guard<std::mutex> lock(mutex_);
//     {

//       // Ignore if already shutting down
//       if (shutting_down_) {
//         return;
//       }
//       shutting_down_ = true;
//       /* Wake up all sleeping threads so they can notice the shutdown flag
//       and
//        exit cleanly.*/
//       cv_.notify_all();
//     }
//   }

// private:
//   std::queue<T> queue_;

//   std::mutex mutex_;

//   std::condition_variable cv_;

//   bool shutting_down_ = false;
// };

// } // namespace kronos

#pragma once

#include <condition_variable>
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

      // Once shutdown begins, the queue is permanently closed to new work.
      // Checking this under the same mutex as queue_.push() makes
      // "is the queue open? -> accept this task" one atomic decision with
      // respect to shutdown().
      if (shutting_down_) {
        return false;
      }

      queue_.push(std::move(task));
    }

    // Wake one sleeping consumer only after the queue mutex is released.
    cv_.notify_one();
    return true;
  }

  std::optional<T> wait_and_pop() {
    std::unique_lock<std::mutex> lock(mutex_);

    // Sleep without busy-waiting until either work arrives or shutdown begins.
    // condition_variable::wait temporarily releases mutex_ while sleeping and
    // reacquires it before returning.
    cv_.wait(lock, [this] { return !queue_.empty() || shutting_down_; });

    // Graceful shutdown: accepted work is drained first. nullopt is returned
    // only when shutdown has started AND there is no queued work left.
    if (shutting_down_ && queue_.empty()) {
      return std::nullopt;
    }

    T task = std::move(queue_.front());
    queue_.pop();
    return task;
  }

  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);

      // Idempotent: calling shutdown() more than once changes nothing.
      if (shutting_down_) {
        return;
      }

      shutting_down_ = true;
    }

    // Wake every waiter after releasing mutex_. Any sleeping consumer can now
    // re-check the predicate, drain accepted work, and eventually return
    // nullopt once the queue becomes empty.
    cv_.notify_all();
  }

private:
  std::queue<T> queue_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool shutting_down_ = false;
};

} // namespace kronos
