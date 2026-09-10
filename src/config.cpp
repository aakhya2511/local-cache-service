#include "cache/config.h"

#include <sys/un.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>

namespace cache {
namespace {

constexpr size_t kMaxKeyBytesHardLimit = 64 * 1024;
constexpr size_t kMaxValueBytesHardLimit = 64ULL * 1024 * 1024;
constexpr size_t kMaxWorkerThreads = 1024;

// Parses a non-negative integer with an optional binary suffix (k/m/g).
bool parse_size(std::string_view text, size_t* out) {
  if (text.empty()) {
    return false;
  }
  size_t multiplier = 1;
  const char suffix = text.back();
  switch (suffix) {
    case 'k': case 'K': multiplier = 1024; text.remove_suffix(1); break;
    case 'm': case 'M': multiplier = 1024ULL * 1024; text.remove_suffix(1); break;
    case 'g': case 'G': multiplier = 1024ULL * 1024 * 1024; text.remove_suffix(1); break;
    default: break;
  }
  if (text.empty()) {
    return false;
  }
  size_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    const size_t digit = static_cast<size_t>(c - '0');
    if (value > (std::numeric_limits<size_t>::max() - digit) / 10) {
      return false;  // overflow
    }
    value = value * 10 + digit;
  }
  if (multiplier > 1 && value > std::numeric_limits<size_t>::max() / multiplier) {
    return false;
  }
  *out = value * multiplier;
  return true;
}

std::string human_bytes(size_t bytes) {
  char buffer[64];
  if (bytes >= 1024ULL * 1024 * 1024) {
    std::snprintf(buffer, sizeof(buffer), "%.2f GiB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
  } else if (bytes >= 1024ULL * 1024) {
    std::snprintf(buffer, sizeof(buffer), "%.0f MiB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    std::snprintf(buffer, sizeof(buffer), "%.0f KiB", static_cast<double>(bytes) / 1024.0);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%zu B", bytes);
  }
  return std::string(buffer);
}

}  // namespace

std::optional<std::string> ServerConfig::validate() const {
  if (socket_path.empty()) {
    return "socket path must not be empty";
  }
  // sun_path is a fixed-size array; bind() would silently truncate longer paths.
  if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
    return "socket path exceeds " + std::to_string(sizeof(sockaddr_un::sun_path) - 1) +
           " bytes (AF_UNIX sun_path limit)";
  }
  if (max_entries == 0) {
    return "--max-entries must be at least 1";
  }
  if (max_value_bytes == 0 || max_value_bytes > kMaxValueBytesHardLimit) {
    return "--max-value-bytes must be in [1, " + std::to_string(kMaxValueBytesHardLimit) + "]";
  }
  if (max_key_bytes == 0 || max_key_bytes > kMaxKeyBytesHardLimit) {
    return "--max-key-bytes must be in [1, " + std::to_string(kMaxKeyBytesHardLimit) + "]";
  }
  if (max_cache_bytes < max_value_bytes + max_key_bytes + LruCache::kEntryOverheadBytes) {
    return "--max-cache-bytes is too small to hold a single maximum-sized entry";
  }
  if (worker_threads == 0 || worker_threads > kMaxWorkerThreads) {
    return "--workers must be in [1, " + std::to_string(kMaxWorkerThreads) + "]";
  }
  if (max_pending_requests == 0) {
    return "--max-pending must be at least 1";
  }
  if (max_connections == 0) {
    return "--max-connections must be at least 1";
  }
  if (max_inflight_per_conn == 0) {
    return "--max-inflight-per-conn must be at least 1";
  }
  if (max_conn_output_bytes < max_value_bytes) {
    return "--max-conn-output-bytes must be at least --max-value-bytes";
  }
  if (accept_backlog <= 0) {
    return "--backlog must be positive";
  }
  if (shutdown_drain_ms < 0) {
    return "--shutdown-drain-ms must not be negative";
  }
  return std::nullopt;
}

void ServerConfig::print_startup_banner() const {
  std::printf("Local Cache Service\n");
  std::printf("  socket:          %s\n", socket_path.c_str());
  std::printf("  workers:         %zu\n", worker_threads);
  std::printf("  max entries:     %zu\n", max_entries);
  std::printf("  cache capacity:  %s\n", human_bytes(max_cache_bytes).c_str());
  std::printf("  max value:       %s\n", human_bytes(max_value_bytes).c_str());
  std::printf("  max key:         %s\n", human_bytes(max_key_bytes).c_str());
  std::printf("  max pending:     %zu\n", max_pending_requests);
  std::printf("  max connections: %zu\n", max_connections);
  std::fflush(stdout);
}

void print_server_usage(const char* program) {
  std::printf(
      "Usage: %s [options]\n"
      "\n"
      "  --socket PATH                 Unix domain socket path (default /tmp/local_cache.sock)\n"
      "  --max-entries N               Maximum number of cache entries\n"
      "  --max-cache-bytes N[k|m|g]    Maximum accounted cache bytes\n"
      "  --max-value-bytes N[k|m|g]    Maximum size of a single value\n"
      "  --max-key-bytes N             Maximum size of a single key\n"
      "  --workers N                   Worker threads handling cache operations\n"
      "  --max-pending N               Bounded work-queue depth (overflow -> SERVER_BUSY)\n"
      "  --max-connections N           Maximum simultaneous client connections\n"
      "  --max-conn-output-bytes N     Per-connection output backlog before read backpressure\n"
      "  --max-inflight-per-conn N     Maximum unanswered requests per connection\n"
      "  --backlog N                   listen() backlog\n"
      "  --shutdown-drain-ms N         Time allowed to drain in-flight work on shutdown\n"
      "  --verbose                     Enable debug logging\n"
      "  --help                        Show this message\n",
      program);
}

ConfigParseResult parse_server_args(int argc, char** argv) {
  ConfigParseResult result;
  ServerConfig config;

  auto fail = [&result](std::string message) {
    result.config.reset();
    result.error = std::move(message);
    return result;
  };

  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      result.help_requested = true;
      return result;
    }
    if (arg == "--verbose") {
      config.verbose = true;
      continue;
    }

    // Accept both "--opt value" and "--opt=value".
    std::string_view name = arg;
    std::string_view value;
    bool have_inline_value = false;
    if (const size_t eq = arg.find('='); eq != std::string_view::npos) {
      name = arg.substr(0, eq);
      value = arg.substr(eq + 1);
      have_inline_value = true;
    } else {
      if (i + 1 >= argc) {
        return fail("missing value for option " + std::string(name));
      }
      value = argv[++i];
    }
    static_cast<void>(have_inline_value);

    auto size_option = [&](size_t* target) -> bool { return parse_size(value, target); };

    bool ok = true;
    if (name == "--socket") {
      config.socket_path = std::string(value);
    } else if (name == "--max-entries") {
      ok = size_option(&config.max_entries);
    } else if (name == "--max-cache-bytes") {
      ok = size_option(&config.max_cache_bytes);
    } else if (name == "--max-value-bytes") {
      ok = size_option(&config.max_value_bytes);
    } else if (name == "--max-key-bytes") {
      ok = size_option(&config.max_key_bytes);
    } else if (name == "--workers") {
      ok = size_option(&config.worker_threads);
    } else if (name == "--max-pending") {
      ok = size_option(&config.max_pending_requests);
    } else if (name == "--max-connections") {
      ok = size_option(&config.max_connections);
    } else if (name == "--max-conn-output-bytes") {
      ok = size_option(&config.max_conn_output_bytes);
    } else if (name == "--max-inflight-per-conn") {
      ok = size_option(&config.max_inflight_per_conn);
    } else if (name == "--backlog") {
      size_t backlog = 0;
      ok = parse_size(value, &backlog) && backlog <= 65535;
      if (ok) {
        config.accept_backlog = static_cast<int>(backlog);
      }
    } else if (name == "--shutdown-drain-ms") {
      size_t drain = 0;
      ok = parse_size(value, &drain) && drain <= 600'000;
      if (ok) {
        config.shutdown_drain_ms = static_cast<int>(drain);
      }
    } else {
      return fail("unknown option " + std::string(name));
    }

    if (!ok) {
      return fail("invalid value for " + std::string(name) + ": " + std::string(value));
    }
  }

  if (auto error = config.validate()) {
    return fail(*error);
  }

  result.config = config;
  return result;
}

}  // namespace cache
