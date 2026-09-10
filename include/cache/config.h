#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "cache/lru_cache.h"

namespace cache {

// Server configuration. Every field has a defensible default so the daemon can
// start with no arguments, and every field is range-checked in validate().
struct ServerConfig {
  std::string socket_path = "/tmp/local_cache.sock";

  // Cache bounds.
  size_t max_entries = 100'000;
  size_t max_cache_bytes = 256ULL * 1024 * 1024;
  size_t max_value_bytes = 1ULL * 1024 * 1024;
  size_t max_key_bytes = 1024;

  // Concurrency.
  size_t worker_threads = 4;
  size_t max_pending_requests = 65'536;

  // Connection limits / backpressure.
  size_t max_connections = 4096;
  // When a connection's unwritten response bytes exceed this, the server stops
  // reading from it until the backlog drains (see DESIGN.md, "Backpressure").
  size_t max_conn_output_bytes = 8ULL * 1024 * 1024;
  // Maximum requests accepted from one connection before its responses are
  // flushed. Bounds per-connection memory held by the worker pool.
  size_t max_inflight_per_conn = 512;
  int accept_backlog = 512;

  // Milliseconds allowed for in-flight work to finish during shutdown.
  int shutdown_drain_ms = 2000;

  bool verbose = false;

  [[nodiscard]] CacheLimits cache_limits() const {
    return CacheLimits{max_entries, max_cache_bytes, max_value_bytes};
  }

  // Returns an error description if any field is out of range.
  [[nodiscard]] std::optional<std::string> validate() const;

  void print_startup_banner() const;
};

struct ConfigParseResult {
  std::optional<ServerConfig> config;  // empty on error or when --help was used
  std::string error;                   // empty when --help was used
  bool help_requested = false;
};

[[nodiscard]] ConfigParseResult parse_server_args(int argc, char** argv);
void print_server_usage(const char* program);

}  // namespace cache
