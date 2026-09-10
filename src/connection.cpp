#include "cache/connection.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace cache {
namespace {

// Size of one read() from a client socket. 64 KiB is large enough to pick up
// many pipelined small requests per syscall without inflating per-connection
// memory (the buffer is a stack temporary, not per-connection state).
constexpr size_t kReadChunkBytes = 64 * 1024;

// Compact the input buffer once the consumed prefix passes this threshold, so
// long-lived pipelined connections do not grow their buffer without bound.
constexpr size_t kCompactThresholdBytes = 32 * 1024;

}  // namespace

Connection::Connection(UniqueFd fd, uint64_t id, ConnectionLimits limits)
    : fd_(std::move(fd)), id_(id), limits_(limits) {}

IoResult Connection::read_some() {
  uint8_t chunk[kReadChunkBytes];
  const ssize_t n = ::recv(fd_.get(), chunk, sizeof(chunk), 0);
  if (n > 0) {
    in_buf_.insert(in_buf_.end(), chunk, chunk + n);
    return IoResult::kOk;
  }
  if (n == 0) {
    return IoResult::kClosed;
  }
  if (errno == EINTR) {
    return IoResult::kOk;  // caller re-enters; level-triggered epoll re-reports
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK) {
    return IoResult::kWouldBlock;
  }
  return IoResult::kError;
}

protocol::ParseResult Connection::next_request(protocol::Request& out) {
  const serializer::ByteView view(in_buf_.data() + in_pos_, in_buf_.size() - in_pos_);
  const protocol::ParseResult result =
      serializer::deserialize_request(view, limits_.protocol_limits, out);
  if (result.status == protocol::ParseStatus::kComplete) {
    in_pos_ += result.consumed;
    if (in_pos_ >= kCompactThresholdBytes) {
      compact_input();
    }
  } else if (result.status == protocol::ParseStatus::kNeedMoreData) {
    compact_input();
  }
  return result;
}

bool Connection::peek_request_header(uint64_t* id, protocol::Opcode* opcode) const {
  const size_t available = in_buf_.size() - in_pos_;
  if (available < protocol::kHeaderSize) {
    return false;
  }
  const uint8_t* head = in_buf_.data() + in_pos_;
  if (serializer::read_u32(head) != protocol::kMagic) {
    return false;  // cannot trust any field of an unsynchronized frame
  }
  const uint8_t raw_opcode = serializer::read_u8(head + 5);
  if (!protocol::is_valid_opcode(raw_opcode)) {
    return false;
  }
  *id = serializer::read_u64(head + 8);
  *opcode = static_cast<protocol::Opcode>(raw_opcode);
  return true;
}

void Connection::compact_input() {
  if (in_pos_ == 0) {
    return;
  }
  in_buf_.erase(in_buf_.begin(), in_buf_.begin() + static_cast<ptrdiff_t>(in_pos_));
  in_pos_ = 0;
}

bool Connection::enqueue_response(const serializer::Bytes& frame, bool completes_request) {
  if (closed()) {
    if (completes_request) {
      inflight_.fetch_sub(1, std::memory_order_relaxed);
    }
    return false;
  }

  bool over_limit = false;
  {
    const std::lock_guard<std::mutex> guard(out_mutex_);
    staged_.insert(staged_.end(), frame.begin(), frame.end());
    const size_t pending =
        pending_output_bytes_.fetch_add(frame.size(), std::memory_order_relaxed) + frame.size();
    // Hard cap: read backpressure normally keeps us well under this, but a
    // client that never reads its socket must not be able to grow the backlog
    // without bound.
    over_limit = pending > limits_.max_output_bytes * 2;
  }
  if (completes_request) {
    inflight_.fetch_sub(1, std::memory_order_relaxed);
  }
  return !over_limit;
}

bool Connection::has_pending_output() const {
  return pending_output_bytes_.load(std::memory_order_relaxed) > 0;
}

bool Connection::read_paused() const {
  return pending_output_bytes_.load(std::memory_order_relaxed) > limits_.max_output_bytes ||
         inflight_limit_reached();
}

IoResult Connection::flush_output() {
  for (;;) {
    if (write_pos_ >= write_buf_.size()) {
      write_buf_.clear();
      write_pos_ = 0;
      const std::lock_guard<std::mutex> guard(out_mutex_);
      if (staged_.empty()) {
        return IoResult::kOk;  // nothing left to write
      }
      write_buf_.swap(staged_);
    }

    const size_t remaining = write_buf_.size() - write_pos_;
    // MSG_NOSIGNAL: a client that disappears mid-write must surface as EPIPE,
    // not as a process-killing SIGPIPE.
    const ssize_t n = ::send(fd_.get(), write_buf_.data() + write_pos_, remaining, MSG_NOSIGNAL);
    if (n > 0) {
      write_pos_ += static_cast<size_t>(n);
      pending_output_bytes_.fetch_sub(static_cast<size_t>(n), std::memory_order_relaxed);
      continue;  // partial write: keep going until EAGAIN or drained
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return IoResult::kWouldBlock;
    }
    return IoResult::kError;
  }
}

}  // namespace cache
