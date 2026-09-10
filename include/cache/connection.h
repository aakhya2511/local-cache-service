#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "cache/protocol.h"
#include "cache/serializer.h"
#include "cache/unique_fd.h"

namespace cache {

struct ConnectionLimits {
  protocol::Limits protocol_limits{};
  size_t max_output_bytes = 8ULL * 1024 * 1024;
  size_t max_inflight = 512;
};

enum class IoResult : uint8_t {
  kOk,          // made progress, socket not exhausted
  kWouldBlock,  // EAGAIN: nothing more to do until epoll says otherwise
  kClosed,      // peer closed cleanly
  kError,       // fatal error on this connection
};

// One client connection.
//
// Threading contract:
//   * The event-loop thread owns the socket and both I/O buffers. Only it may
//     call read_some(), next_request() and flush_output().
//   * Worker threads only call enqueue_response(), which appends to a staging
//     buffer under `out_mutex_` and never touches the socket.
// This is what keeps socket I/O out from under the cache lock and keeps a
// single writer per descriptor.
class Connection {
 public:
  Connection(UniqueFd fd, uint64_t id, ConnectionLimits limits);

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  [[nodiscard]] int fd() const noexcept { return fd_.get(); }
  [[nodiscard]] uint64_t id() const noexcept { return id_; }

  // --- event-loop thread only ------------------------------------------
  // Reads once from the socket into the input buffer.
  IoResult read_some();

  // Extracts the next complete request. Returns kNeedMoreData when the buffer
  // holds only a partial frame, kMalformed when the stream is unrecoverable.
  protocol::ParseResult next_request(protocol::Request& out);

  // Best-effort request id of the frame at the head of the input buffer, for
  // error responses. Only meaningful when the header itself decoded.
  [[nodiscard]] bool peek_request_header(uint64_t* id, protocol::Opcode* opcode) const;

  // Writes as much staged output as the socket accepts.
  IoResult flush_output();

  [[nodiscard]] bool has_pending_output() const;
  [[nodiscard]] bool read_paused() const;

  void mark_close_after_flush() { close_after_flush_ = true; }
  [[nodiscard]] bool close_after_flush() const noexcept { return close_after_flush_; }

  [[nodiscard]] uint32_t registered_events() const noexcept { return registered_events_; }
  void set_registered_events(uint32_t events) noexcept { registered_events_ = events; }

  void note_request_submitted() { inflight_.fetch_add(1, std::memory_order_relaxed); }
  [[nodiscard]] bool inflight_limit_reached() const {
    return inflight_.load(std::memory_order_relaxed) >= limits_.max_inflight;
  }
  [[nodiscard]] size_t inflight() const {
    return inflight_.load(std::memory_order_relaxed);
  }

  // Marks the connection dead. Called by the event loop before dropping its
  // reference; workers that still hold a shared_ptr will discard responses.
  void mark_closed() { closed_.store(true, std::memory_order_release); }
  [[nodiscard]] bool closed() const { return closed_.load(std::memory_order_acquire); }

  // --- any thread --------------------------------------------------------
  // Stages an already-serialized response. Returns false if the connection is
  // closed or its output backlog is over the limit (caller should close it).
  bool enqueue_response(const serializer::Bytes& frame, bool completes_request);

  // True when this connection needs a wakeup pushed to the event loop. Used to
  // collapse many worker completions into one eventfd notification.
  [[nodiscard]] bool try_mark_notified() {
    return !notify_pending_.exchange(true, std::memory_order_acq_rel);
  }
  void clear_notified() { notify_pending_.store(false, std::memory_order_release); }

 private:
  void compact_input();

  UniqueFd fd_;
  uint64_t id_;
  ConnectionLimits limits_;

  // Event-loop-owned state.
  std::vector<uint8_t> in_buf_;
  size_t in_pos_ = 0;
  std::vector<uint8_t> write_buf_;  // bytes currently being written
  size_t write_pos_ = 0;
  uint32_t registered_events_ = 0;
  bool close_after_flush_ = false;

  // Shared with workers.
  mutable std::mutex out_mutex_;
  std::vector<uint8_t> staged_;  // appended by workers, drained by event loop

  std::atomic<size_t> inflight_{0};
  std::atomic<bool> closed_{false};
  std::atomic<bool> notify_pending_{false};
  std::atomic<size_t> pending_output_bytes_{0};
};

}  // namespace cache
