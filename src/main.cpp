#include <cstdio>
#include <cstdlib>
#include <string>

#include "cache/config.h"
#include "cache/logging.h"
#include "cache/server.h"

int main(int argc, char** argv) {
  cache::ConfigParseResult parsed = cache::parse_server_args(argc, argv);
  if (parsed.help_requested) {
    cache::print_server_usage(argv[0]);
    return 0;
  }
  if (!parsed.config) {
    std::fprintf(stderr, "configuration error: %s\n", parsed.error.c_str());
    cache::print_server_usage(argv[0]);
    return 2;
  }

  const cache::ServerConfig config = *parsed.config;
  cache::log::set_level(config.verbose ? cache::log::Level::kDebug : cache::log::Level::kInfo);

  cache::Server server(config);
  std::string error;
  // Must precede start(): worker threads inherit the blocked signal mask.
  if (!server.install_signal_handling(&error)) {
    std::fprintf(stderr, "failed to install signal handling: %s\n", error.c_str());
    return 1;
  }
  if (!server.start(&error)) {
    std::fprintf(stderr, "failed to start server: %s\n", error.c_str());
    return 1;
  }

  config.print_startup_banner();
  server.run();
  return 0;
}
