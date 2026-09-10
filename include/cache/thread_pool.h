#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "cache/protocol.h"

namespace cache {

class Connection;

// One unit of work: a parsed request plus the connection that will receive the
// response. The connection is held by shared_ptr so a worker can never touch a
// destroyed object even if the event loop drops the connection meanwhile.
struct Job {
  std::shared_ptr<Connection> connection;
  protocol::Request request;
};

// Fixed-size worker pool with a bounded queue.
//
// The queue is bounded on purpose: under overload we return SERVER_BUSY to the
// client instead of buffering unbounded work, which is what keeps server memory
// flat when clients outrun the cache.
class WorkerPool {
 public:
  using Handler = std::function<void(Job&)>;

  WorkerPool(size_t thread_count, size_t max_pending, Handler handler);
  ~WorkerPool();

  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  void start();

  // Returns false when the queue is full or the pool is stopping; the caller
  // is responsible for answering the client (SERVER_BUSY).
  [[nodiscard]] bool try_submit(Job&& job);

  // Stops accepting work, waits up to `timeout` for the queue to drain, then
  // joins all workers. Jobs still queued after the timeout are discarded.
  void drain_and_stop(std::chrono::milliseconds timeout);

  [[nodiscard]] size_t pending() const;
  [[nodiscard]] size_t thread_count() const noexcept { return thread_count_; }
  [[nodiscard]] uint64_t completed() const { return completed_.load(std::memory_order_relaxed); }

 private:
  void worker_loop();

  const size_t thread_count_;
  const size_t max_pending_;
  Handler handler_;

  mutable std::mutex mutex_;
  std::condition_variable work_available_;
  std::condition_variable queue_drained_;
  std::deque<Job> queue_;
  bool accepting_ = true;
  bool stopping_ = false;

  std::vector<std::thread> threads_;
  std::atomic<uint64_t> completed_{0};
};

}  // namespace cache
