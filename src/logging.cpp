#include "cache/logging.h"

#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace cache::log {
namespace {

std::atomic<int> g_level{static_cast<int>(Level::kInfo)};

const char* level_name(Level level) noexcept {
  switch (level) {
    case Level::kDebug: return "DEBUG";
    case Level::kInfo: return "INFO ";
    case Level::kWarn: return "WARN ";
    case Level::kError: return "ERROR";
  }
  return "?????";
}

}  // namespace

void set_level(Level level) noexcept {
  g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

Level level() noexcept {
  return static_cast<Level>(g_level.load(std::memory_order_relaxed));
}

bool enabled(Level lvl) noexcept {
  return static_cast<int>(lvl) >= g_level.load(std::memory_order_relaxed);
}

void write_line(Level lvl, std::string_view message) {
  char buffer[1024];
  const int written = std::snprintf(buffer, sizeof(buffer), "%s %.*s\n", level_name(lvl),
                                    static_cast<int>(message.size()), message.data());
  if (written <= 0) {
    return;
  }
  const size_t len = static_cast<size_t>(written) < sizeof(buffer)
                         ? static_cast<size_t>(written)
                         : sizeof(buffer) - 1;
  // Single write() keeps lines atomic across threads for pipe/tty writes below
  // PIPE_BUF; return value is intentionally ignored because a failed log write
  // must never affect request handling.
  const ssize_t ignored = ::write(STDERR_FILENO, buffer, len);
  static_cast<void>(ignored);
}

namespace detail {

void log_formatted(Level lvl, const char* fmt, ...) {
  char message[960];
  va_list args;
  va_start(args, fmt);
  const int n = std::vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  if (n < 0) {
    return;
  }
  write_line(lvl, std::string_view(message));
}

}  // namespace detail
}  // namespace cache::log
