#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "cache_client.h"

namespace {

constexpr const char* kDefaultSocket = "/tmp/local_cache.sock";

void print_usage(const char* program) {
  std::printf(
      "Usage: %s [--socket PATH] <command> [args]\n"
      "\n"
      "Commands:\n"
      "  put <key> <value>   Store a value (value read from stdin if omitted)\n"
      "  get <key>           Fetch a value, written verbatim to stdout\n"
      "  delete <key>        Remove a key\n"
      "  ping                Check that the service is reachable\n"
      "  stats               Print server statistics\n"
      "  clear               Drop every cache entry\n"
      "\n"
      "Exit codes: 0 success, 1 transport error, 3 not found, 4 server error\n",
      program);
}

std::string read_stdin() {
  std::string data;
  char buffer[8192];
  size_t n = 0;
  while ((n = std::fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
    data.append(buffer, n);
  }
  return data;
}

int report_status(cache::protocol::Status status) {
  switch (status) {
    case cache::protocol::Status::kOk:
      return 0;
    case cache::protocol::Status::kNotFound:
      std::fprintf(stderr, "NOT_FOUND\n");
      return 3;
    default:
      std::fprintf(stderr, "%s\n", cache::protocol::to_string(status));
      return 4;
  }
}

void print_stats(const cache::StatsSnapshot& s) {
  std::printf("cache_hits            %llu\n", static_cast<unsigned long long>(s.cache_hits));
  std::printf("cache_misses          %llu\n", static_cast<unsigned long long>(s.cache_misses));
  std::printf("hit_rate              %.4f\n", s.hit_rate());
  std::printf("puts                  %llu\n", static_cast<unsigned long long>(s.puts));
  std::printf("deletes               %llu\n", static_cast<unsigned long long>(s.deletes));
  std::printf("evictions             %llu\n", static_cast<unsigned long long>(s.evictions));
  std::printf("current_entries       %llu\n", static_cast<unsigned long long>(s.current_entries));
  std::printf("current_cache_bytes   %llu\n",
              static_cast<unsigned long long>(s.current_cache_bytes));
  std::printf("current_payload_bytes %llu\n",
              static_cast<unsigned long long>(s.current_payload_bytes));
  std::printf("active_connections    %llu\n",
              static_cast<unsigned long long>(s.active_connections));
  std::printf("total_connections     %llu\n",
              static_cast<unsigned long long>(s.total_connections));
  std::printf("requests_processed    %llu\n",
              static_cast<unsigned long long>(s.requests_processed));
  std::printf("protocol_errors       %llu\n", static_cast<unsigned long long>(s.protocol_errors));
  std::printf("rejected_requests     %llu\n",
              static_cast<unsigned long long>(s.rejected_requests));
  std::printf("uptime_ms             %llu\n", static_cast<unsigned long long>(s.uptime_ms));
}

}  // namespace

int main(int argc, char** argv) {
  std::string socket_path = kDefaultSocket;
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--socket" && i + 1 < argc) {
      socket_path = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    } else {
      args.emplace_back(arg);
    }
  }
  if (args.empty()) {
    print_usage(argv[0]);
    return 2;
  }

  cache::CacheClient client(socket_path);
  if (const std::string error = client.connect(); !error.empty()) {
    std::fprintf(stderr, "connect failed: %s\n", error.c_str());
    return 1;
  }

  const std::string& command = args[0];
  if (command == "put") {
    if (args.size() < 2) {
      std::fprintf(stderr, "put requires a key\n");
      return 2;
    }
    const std::string value = args.size() >= 3 ? args[2] : read_stdin();
    const cache::CacheResult result = client.put(args[1], value);
    if (!result.ok()) {
      std::fprintf(stderr, "put failed: %s\n", result.error.c_str());
      return 1;
    }
    return report_status(result.status);
  }
  if (command == "get") {
    if (args.size() < 2) {
      std::fprintf(stderr, "get requires a key\n");
      return 2;
    }
    const cache::CacheResult result = client.get(args[1]);
    if (!result.ok()) {
      std::fprintf(stderr, "get failed: %s\n", result.error.c_str());
      return 1;
    }
    if (result.status == cache::protocol::Status::kOk) {
      // Values are arbitrary bytes: write them raw, no newline, no assumption
      // that the payload is printable text.
      std::fwrite(result.value.data(), 1, result.value.size(), stdout);
    }
    return report_status(result.status);
  }
  if (command == "delete") {
    if (args.size() < 2) {
      std::fprintf(stderr, "delete requires a key\n");
      return 2;
    }
    const cache::CacheResult result = client.erase(args[1]);
    if (!result.ok()) {
      std::fprintf(stderr, "delete failed: %s\n", result.error.c_str());
      return 1;
    }
    return report_status(result.status);
  }
  if (command == "ping") {
    const cache::CacheResult result = client.ping();
    if (!result.ok()) {
      std::fprintf(stderr, "ping failed: %s\n", result.error.c_str());
      return 1;
    }
    std::printf("PONG\n");
    return report_status(result.status);
  }
  if (command == "clear") {
    const cache::CacheResult result = client.clear();
    if (!result.ok()) {
      std::fprintf(stderr, "clear failed: %s\n", result.error.c_str());
      return 1;
    }
    return report_status(result.status);
  }
  if (command == "stats") {
    const cache::StatsResult result = client.stats();
    if (!result.ok()) {
      std::fprintf(stderr, "stats failed: %s\n", result.error.c_str());
      return 1;
    }
    print_stats(result.stats);
    return 0;
  }

  std::fprintf(stderr, "unknown command: %s\n", command.c_str());
  print_usage(argv[0]);
  return 2;
}
