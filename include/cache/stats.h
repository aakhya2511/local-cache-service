#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace cache {

// Plain snapshot of the counters, safe to copy and serialize.
//
// Field order is part of the wire format for STATS responses; append new
// fields at the end only (the reader tolerates a shorter or longer list).
struct StatsSnapshot {
  uint64_t cache_hits = 0;
  uint64_t cache_misses = 0;
  uint64_t puts = 0;
  uint64_t deletes = 0;
  uint64_t evictions = 0;
  uint64_t current_entries = 0;
  uint64_t current_cache_bytes = 0;
  uint64_t current_payload_bytes = 0;
  uint64_t active_connections = 0;
  uint64_t total_connections = 0;
  uint64_t requests_processed = 0;
  uint64_t protocol_errors = 0;
  uint64_t rejected_requests = 0;
  uint64_t uptime_ms = 0;

  // Derived on presentation rather than stored, so it can never disagree with
  // the counters it is computed from.
  [[nodiscard]] double hit_rate() const {
    const uint64_t lookups = cache_hits + cache_misses;
    return lookups == 0 ? 0.0 : static_cast<double>(cache_hits) / static_cast<double>(lookups);
  }
};

// Live counters. Relaxed ordering throughout: these are statistics, not
// synchronization; no algorithm makes decisions based on them.
class ServerStats {
 public:
  ServerStats() : start_time_(std::chrono::steady_clock::now()) {}

  void record_hit() { hits_.fetch_add(1, std::memory_order_relaxed); }
  void record_miss() { misses_.fetch_add(1, std::memory_order_relaxed); }
  void record_put() { puts_.fetch_add(1, std::memory_order_relaxed); }
  void record_delete() { deletes_.fetch_add(1, std::memory_order_relaxed); }
  void record_request() { requests_.fetch_add(1, std::memory_order_relaxed); }
  void record_protocol_error() { protocol_errors_.fetch_add(1, std::memory_order_relaxed); }
  void record_rejected() { rejected_.fetch_add(1, std::memory_order_relaxed); }
  void connection_opened() {
    active_connections_.fetch_add(1, std::memory_order_relaxed);
    total_connections_.fetch_add(1, std::memory_order_relaxed);
  }
  void connection_closed() { active_connections_.fetch_sub(1, std::memory_order_relaxed); }

  [[nodiscard]] uint64_t active_connections() const {
    return active_connections_.load(std::memory_order_relaxed);
  }

  // Fills everything except the cache-owned fields, which the caller copies in
  // from a LruCache::snapshot() (the cache owns that accounting).
  [[nodiscard]] StatsSnapshot snapshot() const {
    StatsSnapshot s;
    s.cache_hits = hits_.load(std::memory_order_relaxed);
    s.cache_misses = misses_.load(std::memory_order_relaxed);
    s.puts = puts_.load(std::memory_order_relaxed);
    s.deletes = deletes_.load(std::memory_order_relaxed);
    s.active_connections = active_connections_.load(std::memory_order_relaxed);
    s.total_connections = total_connections_.load(std::memory_order_relaxed);
    s.requests_processed = requests_.load(std::memory_order_relaxed);
    s.protocol_errors = protocol_errors_.load(std::memory_order_relaxed);
    s.rejected_requests = rejected_.load(std::memory_order_relaxed);
    s.uptime_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time_)
            .count());
    return s;
  }

 private:
  std::atomic<uint64_t> hits_{0};
  std::atomic<uint64_t> misses_{0};
  std::atomic<uint64_t> puts_{0};
  std::atomic<uint64_t> deletes_{0};
  std::atomic<uint64_t> requests_{0};
  std::atomic<uint64_t> protocol_errors_{0};
  std::atomic<uint64_t> rejected_{0};
  std::atomic<uint64_t> active_connections_{0};
  std::atomic<uint64_t> total_connections_{0};
  std::chrono::steady_clock::time_point start_time_;
};

}  // namespace cache
