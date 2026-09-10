#pragma once

#include <atomic>
#include <string_view>

namespace cache::log {

enum class Level : int { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

// Global minimum level. Deliberately a single relaxed atomic: logging is not
// on the request hot path in normal mode, and a torn read is impossible for an
// int-sized value.
void set_level(Level level) noexcept;
[[nodiscard]] Level level() noexcept;
[[nodiscard]] bool enabled(Level level) noexcept;

// Writes "LEVEL message\n" to stderr with a single write() so concurrent
// logging from workers cannot interleave mid-line.
void write_line(Level level, std::string_view message);

namespace detail {
void log_formatted(Level level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
}  // namespace detail

}  // namespace cache::log

#define LCS_LOG(level_, ...)                                        \
  do {                                                              \
    if (::cache::log::enabled(level_)) {                            \
      ::cache::log::detail::log_formatted((level_), __VA_ARGS__);   \
    }                                                               \
  } while (0)

#define LCS_DEBUG(...) LCS_LOG(::cache::log::Level::kDebug, __VA_ARGS__)
#define LCS_INFO(...) LCS_LOG(::cache::log::Level::kInfo, __VA_ARGS__)
#define LCS_WARN(...) LCS_LOG(::cache::log::Level::kWarn, __VA_ARGS__)
#define LCS_ERROR(...) LCS_LOG(::cache::log::Level::kError, __VA_ARGS__)
