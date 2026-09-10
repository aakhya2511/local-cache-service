#include <gtest/gtest.h>

#include "cache/lru_cache.h"

TEST(MemoryBoundTest, SustainedInsertionsStayWithinBothLimitsAndEvict) {
  constexpr size_t kEntries = 128;
  constexpr size_t kBytes = 48 * 1024;
  cache::LruCache cache(cache::CacheLimits{kEntries, kBytes, 256});
  for (size_t i = 0; i < 100'000; ++i) {
    ASSERT_EQ(cache.put("key:" + std::to_string(i), std::string(200, static_cast<char>(i))),
              cache::PutStatus::kStored);
    if (i % 1000 == 0) {
      const auto snapshot = cache.snapshot();
      ASSERT_LE(snapshot.entries, kEntries);
      ASSERT_LE(snapshot.bytes, kBytes);
    }
  }
  const auto snapshot = cache.snapshot();
  EXPECT_LE(snapshot.entries, kEntries); EXPECT_LE(snapshot.bytes, kBytes);
  EXPECT_GT(snapshot.evictions, 90'000U);
}
