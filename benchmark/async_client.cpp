#include "async_client.h"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <unordered_map>

#include "cache/serializer.h"

namespace bench {
namespace {
using Clock = std::chrono::steady_clock;

std::string error_text(const char* operation) {
  return std::string(operation) + ": " + std::strerror(errno);
}
}  // namespace

struct Engine::Conn {
  struct Pending {
    RequestSpec spec;
    Clock::time_point started;
  };

  int fd = -1;
  size_t index = 0;
  bool live = false;
  std::vector<uint8_t> input;
  std::vector<uint8_t> output;
  size_t output_pos = 0;
  std::unordered_map<uint64_t, Pending> pending;

  ~Conn() {
    if (fd >= 0) ::close(fd);
  }
};

Engine::Engine(EngineConfig config, Generator generator, CompletionHandler on_complete)
    : config_(std::move(config)), generator_(std::move(generator)),
      on_complete_(std::move(on_complete)) {}

Engine::~Engine() {
  if (epoll_fd_ >= 0) ::close(epoll_fd_);
}

std::string Engine::connect_all() {
  if (config_.connections == 0 || config_.pipeline_depth == 0) {
    return "connections and pipeline depth must be positive";
  }
  if (config_.socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
    return "Unix socket path is too long";
  }
  epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) return error_text("epoll_create1");

  conns_.reserve(config_.connections);
  for (size_t i = 0; i < config_.connections; ++i) {
    auto conn = std::make_unique<Conn>();
    conn->index = config_.first_connection + i;
    conn->fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (conn->fd < 0) {
      ++stats_.connect_failures;
      return error_text("socket");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, config_.socket_path.c_str(), config_.socket_path.size());
    if (::connect(conn->fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
      ++stats_.connect_failures;
      return error_text("connect");
    }
    const int flags = ::fcntl(conn->fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(conn->fd, F_SETFL, flags | O_NONBLOCK) != 0) {
      return error_text("fcntl(O_NONBLOCK)");
    }
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP;
    event.data.ptr = conn.get();
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, conn->fd, &event) != 0) {
      return error_text("epoll_ctl");
    }
    conn->live = true;
    ++live_connections_;
    conns_.push_back(std::move(conn));
  }
  return {};
}

bool Engine::fill_pipeline(Conn& conn) {
  while (!generator_exhausted_ && conn.pending.size() < config_.pipeline_depth &&
         issued_ < config_.requests) {
    RequestSpec spec;
    if (!generator_(conn.index, issued_, spec)) {
      generator_exhausted_ = true;
      break;
    }
    const uint64_t id = next_request_id_++;
    cache::protocol::Request request{spec.opcode, id, spec.key, spec.value};
    cache::serializer::serialize_request_into(request, conn.output);
    conn.pending.emplace(id, Conn::Pending{std::move(spec), Clock::now()});
    ++issued_;
    ++stats_.sent;
  }
  return true;
}

bool Engine::on_writable(Conn& conn) {
  fill_pipeline(conn);
  while (conn.output_pos < conn.output.size()) {
    const ssize_t n = ::send(conn.fd, conn.output.data() + conn.output_pos,
                             conn.output.size() - conn.output_pos, MSG_NOSIGNAL);
    if (n > 0) {
      conn.output_pos += static_cast<size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return update_interest(conn, true);
    } else {
      return false;
    }
  }
  conn.output.clear();
  conn.output_pos = 0;
  return update_interest(conn, false);
}

bool Engine::update_interest(Conn& conn, bool want_write) {
  epoll_event event{};
  event.events = EPOLLIN | EPOLLRDHUP | (want_write ? EPOLLOUT : 0U);
  event.data.ptr = &conn;
  return ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, conn.fd, &event) == 0;
}

bool Engine::on_readable(Conn& conn) {
  uint8_t buffer[64 * 1024];
  for (;;) {
    const ssize_t n = ::recv(conn.fd, buffer, sizeof(buffer), 0);
    if (n > 0) {
      conn.input.insert(conn.input.end(), buffer, buffer + n);
    } else if (n == 0) {
      return false;
    } else if (errno == EINTR) {
      continue;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    } else {
      return false;
    }
  }

  size_t consumed = 0;
  while (consumed < conn.input.size()) {
    cache::protocol::Response response;
    const auto view = cache::serializer::ByteView(conn.input).subspan(consumed);
    const auto parsed = cache::serializer::deserialize_response(
        view, cache::protocol::Limits{}, response);
    if (parsed.status == cache::protocol::ParseStatus::kNeedMoreData) break;
    if (parsed.status == cache::protocol::ParseStatus::kMalformed) {
      ++stats_.protocol_failures;
      return false;
    }
    consumed += parsed.consumed;
    auto pending = conn.pending.find(response.request_id);
    if (pending == conn.pending.end()) {
      ++stats_.protocol_failures;
      return false;
    }
    if (response.opcode != pending->second.spec.opcode) {
      ++stats_.protocol_failures;
      return false;
    }
    const uint64_t latency = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - pending->second.started)
            .count());
    Completion completion{conn.index, pending->second.spec, response, latency};
    on_complete_(completion);
    conn.pending.erase(pending);
    ++stats_.received;
  }
  if (consumed != 0) {
    conn.input.erase(conn.input.begin(), conn.input.begin() + static_cast<ptrdiff_t>(consumed));
  }
  fill_pipeline(conn);
  return on_writable(conn);
}

void Engine::drop(Conn& conn) {
  if (!conn.live) return;
  conn.live = false;
  ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, conn.fd, nullptr);
  ::close(conn.fd);
  conn.fd = -1;
  --live_connections_;
  ++stats_.io_failures;
}

bool Engine::all_done() const {
  if (!generator_exhausted_ && issued_ < config_.requests) return false;
  if (stats_.received < issued_) return false;
  for (const auto& conn : conns_) {
    if (!conn->pending.empty() || conn->output_pos < conn->output.size()) return false;
  }
  return true;
}

std::string Engine::run() {
  for (auto& conn : conns_) {
    fill_pipeline(*conn);
    if (!on_writable(*conn)) drop(*conn);
  }
  std::vector<epoll_event> events(256);
  while (!all_done()) {
    if (live_connections_ == 0) return "all benchmark connections failed";
    const int count = ::epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()),
                                   config_.idle_timeout_ms);
    if (count == 0) return "benchmark timed out waiting for server responses";
    if (count < 0) {
      if (errno == EINTR) continue;
      return error_text("epoll_wait");
    }
    for (int i = 0; i < count; ++i) {
      auto& conn = *static_cast<Conn*>(events[static_cast<size_t>(i)].data.ptr);
      if (!conn.live) continue;
      const uint32_t flags = events[static_cast<size_t>(i)].events;
      if ((flags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
        drop(conn);
        continue;
      }
      bool ok = true;
      if ((flags & EPOLLIN) != 0) ok = on_readable(conn);
      if (ok && (flags & EPOLLOUT) != 0) ok = on_writable(conn);
      if (!ok) drop(conn);
    }
  }
  return {};
}

}  // namespace bench
