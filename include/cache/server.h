#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cache/config.h"
#include "cache/connection.h"
#include "cache/lru_cache.h"
#include "cache/stats.h"
#include "cache/thread_pool.h"
#include "cache/unique_fd.h"

namespace cache {

// Unix domain socket cache daemon.
//
// Threading model:
//   1 event-loop thread  - accept(), read(), write(), epoll bookkeeping
//   N worker threads     - parse-to-response handling and cache access
// Workers never touch a socket; they stage bytes on the connection and poke
// the event loop through an eventfd. See DESIGN.md for the rationale.
class Server {
 public:
  explicit Server(ServerConfig config);
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Blocks SIGINT/SIGTERM process-wide and routes them to a signalfd owned by
  // the event loop. Must be called before start() so worker threads inherit
  // the blocked signal mask. Optional: tests drive shutdown programmatically.
  [[nodiscard]] bool install_signal_handling(std::string* error);

  // Creates the socket, binds, listens and starts the worker pool.
  [[nodiscard]] bool start(std::string* error);

  // Runs the event loop until shutdown is requested, then drains and cleans up.
  void run();

  // Thread-safe; may be called from any thread (or from run()'s own loop).
  void request_shutdown();

  [[nodiscard]] StatsSnapshot stats_snapshot() const;
  [[nodiscard]] const ServerConfig& config() const noexcept { return config_; }
  [[nodiscard]] LruCache& cache() noexcept { return cache_; }

 private:
  void handle_listen_ready();
  void handle_connection_event(const std::shared_ptr<Connection>& conn, uint32_t events);
  // Returns false if the connection was closed.
  bool process_input(const std::shared_ptr<Connection>& conn);
  void handle_job(Job& job);
  void complete_job(const std::shared_ptr<Connection>& conn, const serializer::Bytes& frame);
  void drain_completions();
  void update_interest(const std::shared_ptr<Connection>& conn);
  void close_connection(const std::shared_ptr<Connection>& conn);
  void stop_listening();
  void send_immediate(const std::shared_ptr<Connection>& conn, protocol::Opcode opcode,
                      uint64_t request_id, protocol::Status status);
  [[nodiscard]] bool drain_complete() const;

  ServerConfig config_;
  LruCache cache_;
  ServerStats stats_;
  std::unique_ptr<WorkerPool> pool_;

  UniqueFd epoll_fd_;
  UniqueFd listen_fd_;
  UniqueFd shutdown_fd_;    // eventfd: shutdown request
  UniqueFd completion_fd_;  // eventfd: worker responses ready to write
  UniqueFd signal_fd_;      // signalfd: SIGINT/SIGTERM (optional)

  std::unordered_map<int, std::shared_ptr<Connection>> connections_;
  uint64_t next_connection_id_ = 1;
  std::string bound_socket_path_;  // non-empty once we own the socket file

  std::mutex ready_mutex_;
  std::vector<std::shared_ptr<Connection>> ready_connections_;

  bool running_ = false;
  bool draining_ = false;
};

}  // namespace cache
