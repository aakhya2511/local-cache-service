#include "cache/server.h"

#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>

#include "cache/logging.h"
#include "cache/serializer.h"

namespace cache {
namespace {

// Level-triggered epoll is used throughout (see DESIGN.md): with LT we can do
// exactly one read() per readiness notification and let the loop come back for
// the rest, which bounds the work one busy connection can do per iteration and
// removes the "must drain until EAGAIN or lose the event" failure mode of ET.
constexpr uint32_t kBaseConnectionEvents = EPOLLRDHUP;

// Poll interval while draining so the drain deadline is honoured even if no
// events arrive.
constexpr int kDrainPollMs = 20;

std::string errno_message(std::string_view what) {
  const int saved_errno = errno;
  return std::string(what) + ": " +
         std::error_code(saved_errno, std::generic_category()).message();
}

// A socket file left behind by a crashed daemon should be replaced, but a
// socket belonging to a *running* daemon must not be: unlinking it would
// silently steal the endpoint. Distinguish the two by trying to connect.
bool remove_stale_socket(const std::string& path, std::string* error) {
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0) {
    if (errno == ENOENT) {
      return true;  // nothing to clean up
    }
    *error = errno_message("stat(" + path + ")");
    return false;
  }
  if (!S_ISSOCK(info.st_mode)) {
    *error = path + " exists and is not a socket; refusing to unlink it";
    return false;
  }

  UniqueFd probe(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!probe) {
    *error = errno_message("socket() while probing stale socket");
    return false;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size());
  if (::connect(probe.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
    *error = "another cache server is already listening on " + path;
    return false;
  }
  if (errno != ECONNREFUSED && errno != ENOENT) {
    *error = errno_message("connect() while probing " + path);
    return false;
  }
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    *error = errno_message("unlink(" + path + ")");
    return false;
  }
  LCS_INFO("removed stale socket %s", path.c_str());
  return true;
}

}  // namespace

Server::Server(ServerConfig config) : config_(std::move(config)), cache_(config_.cache_limits()) {}

Server::~Server() {
  if (pool_) {
    pool_->drain_and_stop(std::chrono::milliseconds(0));
  }
  stop_listening();
}

bool Server::install_signal_handling(std::string* error) {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  // Block in this thread before workers are created so every thread inherits
  // the mask and only the signalfd ever observes these signals. This keeps all
  // shutdown logic out of async-signal context.
  if (const int rc = ::pthread_sigmask(SIG_BLOCK, &mask, nullptr); rc != 0) {
    errno = rc;
    *error = errno_message("pthread_sigmask");
    return false;
  }
  signal_fd_.reset(::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
  if (!signal_fd_) {
    *error = errno_message("signalfd");
    return false;
  }
  return true;
}

bool Server::start(std::string* error) {
  if (auto problem = config_.validate()) {
    *error = *problem;
    return false;
  }
  if (!remove_stale_socket(config_.socket_path, error)) {
    return false;
  }

  listen_fd_.reset(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!listen_fd_) {
    *error = errno_message("socket");
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, config_.socket_path.c_str(), config_.socket_path.size());
  if (::bind(listen_fd_.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    *error = errno_message("bind(" + config_.socket_path + ")");
    return false;
  }
  bound_socket_path_ = config_.socket_path;

  // Access control for a Unix socket is filesystem permissions; restrict the
  // endpoint to the owning user rather than relying on it being "local".
  if (::chmod(config_.socket_path.c_str(), S_IRUSR | S_IWUSR) != 0) {
    *error = errno_message("chmod(" + config_.socket_path + ")");
    return false;
  }
  if (::listen(listen_fd_.get(), config_.accept_backlog) != 0) {
    *error = errno_message("listen");
    return false;
  }

  epoll_fd_.reset(::epoll_create1(EPOLL_CLOEXEC));
  if (!epoll_fd_) {
    *error = errno_message("epoll_create1");
    return false;
  }
  shutdown_fd_.reset(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
  completion_fd_.reset(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
  if (!shutdown_fd_ || !completion_fd_) {
    *error = errno_message("eventfd");
    return false;
  }

  auto register_fd = [this, error](int fd) {
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = fd;
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &event) != 0) {
      *error = errno_message("epoll_ctl(ADD)");
      return false;
    }
    return true;
  };
  if (!register_fd(listen_fd_.get()) || !register_fd(shutdown_fd_.get()) ||
      !register_fd(completion_fd_.get())) {
    return false;
  }
  if (signal_fd_ && !register_fd(signal_fd_.get())) {
    return false;
  }

  pool_ = std::make_unique<WorkerPool>(config_.worker_threads, config_.max_pending_requests,
                                       [this](Job& job) { handle_job(job); });
  pool_->start();
  running_ = true;
  LCS_INFO("server started on %s (workers=%zu)", config_.socket_path.c_str(),
           config_.worker_threads);
  return true;
}

void Server::request_shutdown() {
  if (!shutdown_fd_) {
    return;
  }
  const uint64_t one = 1;
  const ssize_t n = ::write(shutdown_fd_.get(), &one, sizeof(one));
  static_cast<void>(n);  // EAGAIN only if the counter saturated, i.e. already signalled
}

StatsSnapshot Server::stats_snapshot() const {
  StatsSnapshot snapshot = stats_.snapshot();
  const CacheSnapshot cache_state = cache_.snapshot();
  snapshot.current_entries = cache_state.entries;
  snapshot.current_cache_bytes = cache_state.bytes;
  snapshot.current_payload_bytes = cache_state.payload_bytes;
  snapshot.evictions = cache_state.evictions;
  return snapshot;
}

void Server::run() {
  constexpr int kMaxEvents = 256;
  epoll_event events[kMaxEvents];
  std::chrono::steady_clock::time_point drain_deadline{};

  while (running_) {
    const int timeout = draining_ ? kDrainPollMs : -1;
    const int count = ::epoll_wait(epoll_fd_.get(), events, kMaxEvents, timeout);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      LCS_ERROR("%s", errno_message("epoll_wait").c_str());
      break;
    }

    for (int i = 0; i < count; ++i) {
      const int fd = events[i].data.fd;
      if (fd == listen_fd_.get() && listen_fd_) {
        handle_listen_ready();
      } else if (fd == completion_fd_.get()) {
        drain_completions();
      } else if (fd == shutdown_fd_.get()) {
        uint64_t counter = 0;
        const ssize_t n = ::read(shutdown_fd_.get(), &counter, sizeof(counter));
        static_cast<void>(n);
        if (!draining_) {
          LCS_INFO("shutdown requested");
          draining_ = true;
          drain_deadline = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(config_.shutdown_drain_ms);
          stop_listening();
          // Existing clients may still have readable data. Remove EPOLLIN now
          // so shutdown drains only requests that were already submitted.
          std::vector<std::shared_ptr<Connection>> open_connections;
          open_connections.reserve(connections_.size());
          for (const auto& [connection_fd, connection] : connections_) {
            static_cast<void>(connection_fd);
            open_connections.push_back(connection);
          }
          for (const auto& connection : open_connections) {
            update_interest(connection);
          }
        }
      } else if (signal_fd_ && fd == signal_fd_.get()) {
        signalfd_siginfo info{};
        while (::read(signal_fd_.get(), &info, sizeof(info)) == sizeof(info)) {
          LCS_INFO("received signal %u", info.ssi_signo);
        }
        request_shutdown();
      } else {
        const auto it = connections_.find(fd);
        if (it != connections_.end()) {
          // Copy the shared_ptr: the handler may close and erase the entry.
          const std::shared_ptr<Connection> conn = it->second;
          handle_connection_event(conn, events[i].events);
        }
      }
    }

    if (draining_ && (drain_complete() || std::chrono::steady_clock::now() >= drain_deadline)) {
      running_ = false;
    }
  }

  // Ordered teardown: no new work, finish workers, then release sockets.
  stop_listening();
  if (pool_) {
    pool_->drain_and_stop(std::chrono::milliseconds(config_.shutdown_drain_ms));
  }
  drain_completions();  // last chance to write already-produced responses
  for (auto& [fd, conn] : connections_) {
    static_cast<void>(fd);
    conn->mark_closed();
    stats_.connection_closed();
  }
  connections_.clear();
  LCS_INFO("server stopped");
}

bool Server::drain_complete() const {
  if (pool_ && pool_->pending() > 0) {
    return false;
  }
  for (const auto& [fd, conn] : connections_) {
    static_cast<void>(fd);
    if (conn->inflight() > 0 || conn->has_pending_output()) {
      return false;
    }
  }
  return true;
}

void Server::stop_listening() {
  if (listen_fd_) {
    if (epoll_fd_) {
      ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, listen_fd_.get(), nullptr);
    }
    listen_fd_.reset();
  }
  if (!bound_socket_path_.empty()) {
    if (::unlink(bound_socket_path_.c_str()) != 0 && errno != ENOENT) {
      LCS_WARN("%s", errno_message("unlink(" + bound_socket_path_ + ")").c_str());
    }
    bound_socket_path_.clear();
  }
}

void Server::handle_listen_ready() {
  for (;;) {
    UniqueFd client(::accept4(listen_fd_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
    if (!client) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      if (errno == EINTR || errno == ECONNABORTED) {
        continue;
      }
      // EMFILE/ENFILE: returning leaves the connection pending. Level-triggered
      // epoll will retry on the next loop iteration; there is nothing better to
      // do than log and let the descriptor pressure resolve.
      LCS_ERROR("%s", errno_message("accept4").c_str());
      return;
    }

    if (connections_.size() >= config_.max_connections) {
      // Accept-then-close rather than leaving it queued, which would make the
      // level-triggered listener spin.
      LCS_WARN("connection limit %zu reached; rejecting client", config_.max_connections);
      stats_.record_rejected();
      continue;
    }

    ConnectionLimits limits;
    limits.protocol_limits.max_key_bytes = static_cast<uint32_t>(config_.max_key_bytes);
    limits.protocol_limits.max_value_bytes = static_cast<uint32_t>(config_.max_value_bytes);
    limits.max_output_bytes = config_.max_conn_output_bytes;
    limits.max_inflight = config_.max_inflight_per_conn;

    const int fd = client.get();
    auto conn = std::make_shared<Connection>(std::move(client), next_connection_id_++, limits);

    epoll_event event{};
    event.events = EPOLLIN | kBaseConnectionEvents;
    event.data.fd = fd;
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &event) != 0) {
      LCS_ERROR("%s", errno_message("epoll_ctl(ADD client)").c_str());
      continue;  // conn destructor closes the descriptor
    }
    conn->set_registered_events(event.events);
    connections_.emplace(fd, std::move(conn));
    stats_.connection_opened();
  }
}

void Server::handle_connection_event(const std::shared_ptr<Connection>& conn, uint32_t events) {
  if ((events & (EPOLLERR | EPOLLHUP)) != 0) {
    close_connection(conn);
    return;
  }
  if (draining_ && (events & EPOLLRDHUP) != 0) {
    close_connection(conn);
    return;
  }

  if ((events & EPOLLOUT) != 0) {
    if (conn->flush_output() == IoResult::kError) {
      close_connection(conn);
      return;
    }
  }

  if (!draining_ && (events & (EPOLLIN | EPOLLRDHUP)) != 0) {
    // Exactly one read per readiness event: bounded work per connection, and
    // level-triggered epoll will report the rest on the next iteration.
    const IoResult result = conn->read_some();
    if (result == IoResult::kClosed || result == IoResult::kError) {
      // Drop immediately: any in-flight responses become undeliverable.
      close_connection(conn);
      return;
    }
    if (result == IoResult::kOk && !process_input(conn)) {
      return;
    }
  }

  if (conn->flush_output() == IoResult::kError) {
    close_connection(conn);
    return;
  }
  if (conn->close_after_flush() && !conn->has_pending_output()) {
    close_connection(conn);
    return;
  }
  update_interest(conn);
}

bool Server::process_input(const std::shared_ptr<Connection>& conn) {
  for (;;) {
    if (conn->inflight_limit_reached()) {
      return true;  // backpressure: stop pulling requests until responses drain
    }

    protocol::Request request;
    const protocol::ParseResult result = conn->next_request(request);
    if (result.status == protocol::ParseStatus::kNeedMoreData) {
      return true;
    }
    if (result.status == protocol::ParseStatus::kMalformed) {
      stats_.record_protocol_error();
      LCS_WARN("malformed client frame on connection %llu: %s",
               static_cast<unsigned long long>(conn->id()), protocol::to_string(result.error));
      // A length-prefixed stream cannot be resynchronized after a bad frame:
      // answer if we can identify the request, then close.
      uint64_t request_id = 0;
      protocol::Opcode request_opcode = protocol::Opcode::kPing;
      const bool identified = conn->peek_request_header(&request_id, &request_opcode);
      const protocol::Status status = result.error == protocol::ParseError::kValueTooLarge
                                          ? protocol::Status::kValueTooLarge
                                      : result.error == protocol::ParseError::kUnsupportedVersion
                                          ? protocol::Status::kUnsupportedVersion
                                          : protocol::Status::kInvalidRequest;
      if (identified) {
        send_immediate(conn, request_opcode, request_id, status);
      }
      conn->mark_close_after_flush();
      return true;
    }

    const protocol::Opcode opcode = request.opcode;
    const uint64_t request_id = request.request_id;
    stats_.record_request();
    conn->note_request_submitted();

    Job job{conn, std::move(request)};
    if (!pool_->try_submit(std::move(job))) {
      // Bounded queue is full: shed load explicitly instead of growing memory.
      stats_.record_rejected();
      LCS_WARN("pending request queue saturated; rejecting request");
      const serializer::Bytes frame = serializer::serialize_response(
          protocol::Response{opcode, protocol::Status::kServerBusy, request_id, {}});
      if (!conn->enqueue_response(frame, /*completes_request=*/true)) {
        close_connection(conn);
        return false;
      }
    }
  }
}

void Server::send_immediate(const std::shared_ptr<Connection>& conn, protocol::Opcode opcode,
                            uint64_t request_id, protocol::Status status) {
  const serializer::Bytes frame =
      serializer::serialize_response(protocol::Response{opcode, status, request_id, {}});
  if (!conn->enqueue_response(frame, /*completes_request=*/false)) {
    close_connection(conn);
  }
}

void Server::handle_job(Job& job) {
  const protocol::Request& request = job.request;
  protocol::Response response;
  response.opcode = request.opcode;
  response.request_id = request.request_id;
  response.status = protocol::Status::kOk;

  switch (request.opcode) {
    case protocol::Opcode::kGet: {
      if (cache_.get(request.key, response.value)) {
        stats_.record_hit();
      } else {
        response.status = protocol::Status::kNotFound;
        response.value.clear();
        stats_.record_miss();
      }
      break;
    }
    case protocol::Opcode::kPut: {
      switch (cache_.put(request.key, request.value)) {
        case PutStatus::kStored:
          stats_.record_put();
          break;
        case PutStatus::kValueTooLarge:
        case PutStatus::kEntryTooLarge:
          response.status = protocol::Status::kValueTooLarge;
          break;
      }
      break;
    }
    case protocol::Opcode::kDelete: {
      if (cache_.erase(request.key)) {
        stats_.record_delete();
      } else {
        response.status = protocol::Status::kNotFound;
      }
      break;
    }
    case protocol::Opcode::kPing:
      break;
    case protocol::Opcode::kStats: {
      const StatsSnapshot snapshot = stats_snapshot();
      const serializer::Bytes payload = serializer::serialize_stats(snapshot);
      response.value.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
      break;
    }
    case protocol::Opcode::kClear:
      cache_.clear();
      break;
  }

  // Reused per worker thread: response framing costs no allocation in steady
  // state once the buffer has grown to its working size.
  thread_local serializer::Bytes frame;
  frame.clear();
  serializer::serialize_response_into(response, frame);
  complete_job(job.connection, frame);
}

void Server::complete_job(const std::shared_ptr<Connection>& conn,
                          const serializer::Bytes& frame) {
  const bool healthy = conn->enqueue_response(frame, /*completes_request=*/true);
  if (!healthy) {
    // Output backlog blew past the hard cap (a client that stopped reading).
    conn->mark_close_after_flush();
  }
  if (!conn->try_mark_notified()) {
    return;  // a wakeup for this connection is already queued
  }
  {
    const std::lock_guard<std::mutex> guard(ready_mutex_);
    ready_connections_.push_back(conn);
  }
  const uint64_t one = 1;
  const ssize_t n = ::write(completion_fd_.get(), &one, sizeof(one));
  static_cast<void>(n);
}

void Server::drain_completions() {
  uint64_t counter = 0;
  const ssize_t n = ::read(completion_fd_.get(), &counter, sizeof(counter));
  static_cast<void>(n);

  std::vector<std::shared_ptr<Connection>> ready;
  {
    const std::lock_guard<std::mutex> guard(ready_mutex_);
    ready.swap(ready_connections_);
  }

  for (const std::shared_ptr<Connection>& conn : ready) {
    // Clear before flushing: a worker that stages bytes after this point will
    // re-queue the connection, so no wakeup can be lost.
    conn->clear_notified();
    if (conn->closed()) {
      continue;
    }
    if (conn->flush_output() == IoResult::kError) {
      close_connection(conn);
      continue;
    }
    if (conn->close_after_flush() && !conn->has_pending_output()) {
      close_connection(conn);
      continue;
    }
    update_interest(conn);
  }
}

void Server::update_interest(const std::shared_ptr<Connection>& conn) {
  uint32_t events = kBaseConnectionEvents;
  if (!draining_ && !conn->read_paused() && !conn->close_after_flush()) {
    events |= EPOLLIN;
  }
  if (conn->has_pending_output()) {
    events |= EPOLLOUT;
  }
  // Note: (events == kBaseConnectionEvents) is reachable only while requests
  // are in flight, and every completion re-runs this function, so a connection
  // can never be parked forever with no interest registered.
  if (events == conn->registered_events()) {
    return;
  }
  epoll_event event{};
  event.events = events;
  event.data.fd = conn->fd();
  if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, conn->fd(), &event) != 0) {
    LCS_ERROR("%s", errno_message("epoll_ctl(MOD)").c_str());
    close_connection(conn);
    return;
  }
  conn->set_registered_events(events);
}

void Server::close_connection(const std::shared_ptr<Connection>& conn) {
  if (conn->closed()) {
    return;  // idempotent: several code paths can notice the same failure
  }
  const int fd = conn->fd();
  if (fd >= 0 && epoll_fd_) {
    ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, fd, nullptr);
  }
  // Mark closed first so any worker still holding this connection discards its
  // response instead of staging bytes for a dead socket.
  conn->mark_closed();
  connections_.erase(fd);
  stats_.connection_closed();
  LCS_DEBUG("connection %llu closed", static_cast<unsigned long long>(conn->id()));
}

}  // namespace cache
