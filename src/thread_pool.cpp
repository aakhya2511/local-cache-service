#include "cache/thread_pool.h"

#include <utility>

#include "cache/logging.h"

namespace cache {

WorkerPool::WorkerPool(size_t thread_count, size_t max_pending, Handler handler)
    : thread_count_(thread_count), max_pending_(max_pending), handler_(std::move(handler)) {}

WorkerPool::~WorkerPool() { drain_and_stop(std::chrono::milliseconds(0)); }

void WorkerPool::start() {
  threads_.reserve(thread_count_);
  for (size_t i = 0; i < thread_count_; ++i) {
    threads_.emplace_back([this] { worker_loop(); });
  }
}

bool WorkerPool::try_submit(Job&& job) {
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (!accepting_ || queue_.size() >= max_pending_) {
      return false;
    }
    queue_.push_back(std::move(job));
  }
  work_available_.notify_one();
  return true;
}

size_t WorkerPool::pending() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return queue_.size();
}

void WorkerPool::worker_loop() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      work_available_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (stopping_) {
          return;
        }
        continue;  // spurious wakeup
      }
      job = std::move(queue_.front());
      queue_.pop_front();
      if (queue_.empty()) {
        queue_drained_.notify_all();
      }
    }
    // The handler runs without any pool lock held, so cache work and response
    // staging never serialize on the queue mutex.
    handler_(job);
    completed_.fetch_add(1, std::memory_order_relaxed);
  }
}

void WorkerPool::drain_and_stop(std::chrono::milliseconds timeout) {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!accepting_ && stopping_) {
      return;  // already stopped
    }
    accepting_ = false;
    if (timeout.count() > 0 && !queue_.empty()) {
      const bool drained =
          queue_drained_.wait_for(lock, timeout, [this] { return queue_.empty(); });
      if (!drained) {
        LCS_WARN("shutdown: discarding %zu queued requests after drain timeout", queue_.size());
        queue_.clear();
      }
    }
    stopping_ = true;
  }
  work_available_.notify_all();
  for (std::thread& thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  threads_.clear();
}

}  // namespace cache
