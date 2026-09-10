#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cache/protocol.h"

namespace bench {

// What a generator asks the engine to send.
struct RequestSpec {
  cache::protocol::Opcode opcode = cache::protocol::Opcode::kPing;
  std::string key;
  std::string value;
};

struct Completion {
  size_t connection_index = 0;
  const RequestSpec& request;
  const cache::protocol::Response& response;
  uint64_t latency_ns = 0;
};

struct EngineConfig {
  std::string socket_path;
  size_t connections = 1;
  size_t pipeline_depth = 1;   // outstanding requests per connection
  uint64_t requests = 0;       // total requests this engine will issue
  size_t first_connection = 0; // global index of this engine's first connection
  int idle_timeout_ms = 30'000;
};

struct EngineStats {
  uint64_t sent = 0;
  uint64_t received = 0;
  uint64_t connect_failures = 0;
  uint64_t io_failures = 0;
  uint64_t protocol_failures = 0;
};

// Single-threaded, epoll-driven, pipelining client.
//
// This is what makes "10,000 in-flight requests" possible without 10,000
// threads: `connections` sockets each keep `pipeline_depth` requests
// outstanding, and one thread multiplexes all of them.
//
// Callbacks:
//   generator(connection_index, sequence, spec) -> false to stop early
//   on_complete(completion)                     -> called per response
// Both run on the engine's own thread.
class Engine {
 public:
  using Generator = std::function<bool(size_t, uint64_t, RequestSpec&)>;
  using CompletionHandler = std::function<void(const Completion&)>;

  Engine(EngineConfig config, Generator generator, CompletionHandler on_complete);
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Connects every socket up front; returns an error string on failure.
  [[nodiscard]] std::string connect_all();

  // Drives the event loop until all requests are answered or an error occurs.
  [[nodiscard]] std::string run();

  [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }
  [[nodiscard]] size_t connection_count() const noexcept { return config_.connections; }

 private:
  struct Conn;

  // Returns false if the connection failed and was dropped.
  bool fill_pipeline(Conn& conn);
  bool on_readable(Conn& conn);
  bool on_writable(Conn& conn);
  bool update_interest(Conn& conn, bool want_write);
  void drop(Conn& conn);
  [[nodiscard]] bool all_done() const;

  EngineConfig config_;
  Generator generator_;
  CompletionHandler on_complete_;
  EngineStats stats_{};
  std::vector<std::unique_ptr<Conn>> conns_;
  uint64_t issued_ = 0;   // requests handed to the socket layer
  uint64_t next_request_id_ = 1;
  bool generator_exhausted_ = false;
  int epoll_fd_ = -1;
  size_t live_connections_ = 0;
};

}  // namespace bench
