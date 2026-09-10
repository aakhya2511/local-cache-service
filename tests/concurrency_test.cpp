#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "test_server.h"

TEST(ConcurrencyTest, ParallelClientsPreserveBinaryPayloads) {
  RunningServer server;
  constexpr size_t kThreads = 24;
  constexpr size_t kOperations = 300;
  std::atomic<uint64_t> failures{0};
  std::vector<std::thread> threads;
  for (size_t thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&, thread] {
      auto client = server.client();
      for (size_t i = 0; i < kOperations; ++i) {
        const std::string key = "t:" + std::to_string(thread) + ":" + std::to_string(i);
        std::string value = key;
        value.append("\0\xff", 2);
        const auto put = client.put(key, value);
        const auto get = client.get(key);
        if (!put.ok() || put.status != cache::protocol::Status::kOk || !get.ok() ||
            get.status != cache::protocol::Status::kOk || get.value != value) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        if (i % 5 == 0 && client.erase(key).status != cache::protocol::Status::kOk) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(failures.load(), 0U);
}

TEST(ConcurrencyTest, SharedCacheMaintainsAccountingUnderContention) {
  cache::LruCache cache(cache::CacheLimits{200, 64 * 1024, 512});
  std::vector<std::thread> threads;
  for (size_t t = 0; t < 16; ++t) {
    threads.emplace_back([&, t] {
      uint64_t state = 1234 + t;
      for (size_t i = 0; i < 5000; ++i) {
        state = state * 6364136223846793005ULL + 1;
        const std::string key = "k" + std::to_string(state % 500);
        switch (state % 3) {
          case 0: cache.put(key, std::string(static_cast<size_t>(state % 128), 'x')); break;
          case 1: static_cast<void>(cache.get(key)); break;
          default: static_cast<void>(cache.erase(key)); break;
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  const auto snapshot = cache.snapshot();
  EXPECT_LE(snapshot.entries, 200U); EXPECT_LE(snapshot.bytes, 64U * 1024U);
  EXPECT_GE(snapshot.bytes, snapshot.payload_bytes);
  EXPECT_EQ(cache.keys_mru_to_lru().size(), snapshot.entries);
}
