#include <gtest/gtest.h>

#include "cache/lru_cache.h"

using cache::CacheLimits;
using cache::LruCache;
using cache::PutStatus;

TEST(LruCacheTest, InsertGetUpdateDeleteAndBinaryValues) {
  LruCache cache(CacheLimits{3, 4096, 1024});
  EXPECT_EQ(cache.put("a", std::string_view("x\0y", 3)), PutStatus::kStored);
  EXPECT_EQ(cache.get("a"), std::optional<std::string>(std::string("x\0y", 3)));
  EXPECT_EQ(cache.put("a", "new"), PutStatus::kStored);
  EXPECT_EQ(cache.size(), 1U);
  EXPECT_EQ(cache.get("a"), std::optional<std::string>("new"));
  EXPECT_TRUE(cache.erase("a"));
  EXPECT_FALSE(cache.erase("a"));
}

TEST(LruCacheTest, GetAndUpdatePromoteBeforeEviction) {
  LruCache cache(CacheLimits{3, 4096, 1024});
  cache.put("a", "1"); cache.put("b", "2"); cache.put("c", "3");
  EXPECT_TRUE(cache.get("a").has_value());
  cache.put("d", "4");
  EXPECT_FALSE(cache.contains("b"));
  cache.put("c", "updated");
  cache.put("e", "5");
  EXPECT_FALSE(cache.contains("a"));
  EXPECT_EQ(cache.keys_mru_to_lru(), (std::vector<std::string>{"e", "c", "d"}));
}

TEST(LruCacheTest, EnforcesEntryByteAndValueLimits) {
  const size_t overhead = LruCache::kEntryOverheadBytes;
  LruCache cache(CacheLimits{20, 2 * (overhead + 11), 10});
  EXPECT_EQ(cache.put("a", std::string(10, 'a')), PutStatus::kStored);
  EXPECT_EQ(cache.put("b", std::string(10, 'b')), PutStatus::kStored);
  EXPECT_EQ(cache.put("c", std::string(10, 'c')), PutStatus::kStored);
  EXPECT_LE(cache.memory_usage(), 2 * (overhead + 11));
  EXPECT_EQ(cache.size(), 2U);
  EXPECT_GT(cache.snapshot().evictions, 0U);
  EXPECT_EQ(cache.put("x", std::string(11, 'x')), PutStatus::kValueTooLarge);
}

TEST(LruCacheTest, EmptyKeyAndValueAreSupportedByCoreAndClearResetsUsage) {
  LruCache cache(CacheLimits{2, 1024, 10});
  EXPECT_EQ(cache.put("", ""), PutStatus::kStored);
  EXPECT_TRUE(cache.contains(""));
  cache.clear();
  EXPECT_EQ(cache.size(), 0U);
  EXPECT_EQ(cache.memory_usage(), 0U);
}
