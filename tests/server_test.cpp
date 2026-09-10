#include <gtest/gtest.h>

#include "test_server.h"

TEST(ServerTest, PutGetDeleteMissingAndStats) {
  RunningServer server;
  auto client = server.client();
  const std::string binary("hello\0world", 11);
  ASSERT_EQ(client.put("key", binary).status, cache::protocol::Status::kOk);
  const auto found = client.get("key");
  ASSERT_TRUE(found.ok()); EXPECT_EQ(found.status, cache::protocol::Status::kOk);
  EXPECT_EQ(found.value, binary);
  EXPECT_EQ(client.erase("key").status, cache::protocol::Status::kOk);
  EXPECT_EQ(client.get("key").status, cache::protocol::Status::kNotFound);
  EXPECT_EQ(client.ping().status, cache::protocol::Status::kOk);
  const auto stats = client.stats();
  ASSERT_TRUE(stats.ok());
  EXPECT_GE(stats.stats.cache_hits, 1U); EXPECT_GE(stats.stats.cache_misses, 1U);
  EXPECT_GE(stats.stats.requests_processed, 6U);
}

TEST(ServerTest, MultipleConnectionsAndRestartAtSamePath) {
  cache::ServerConfig config;
  const std::string path = "/tmp/lcs_restart_" + std::to_string(::getpid()) + ".sock";
  config.socket_path = path;
  {
    cache::Server server(config); std::string error;
    ASSERT_TRUE(server.start(&error)) << error;
    std::thread runner([&] { server.run(); });
    cache::CacheClient a(path), b(path);
    ASSERT_TRUE(a.connect().empty()); ASSERT_TRUE(b.connect().empty());
    EXPECT_EQ(a.put("shared", "value").status, cache::protocol::Status::kOk);
    EXPECT_EQ(b.get("shared").value, "value");
    server.request_shutdown(); runner.join();
  }
  {
    cache::Server server(config); std::string error;
    ASSERT_TRUE(server.start(&error)) << error;
    std::thread runner([&] { server.run(); });
    cache::CacheClient client(path); ASSERT_TRUE(client.connect().empty());
    EXPECT_EQ(client.ping().status, cache::protocol::Status::kOk);
    server.request_shutdown(); runner.join();
  }
}

TEST(ServerTest, RejectsOversizedValueAtProtocolBoundary) {
  cache::ServerConfig config; config.max_value_bytes = 64;
  config.max_cache_bytes = config.max_value_bytes + config.max_key_bytes +
                           cache::LruCache::kEntryOverheadBytes;
  RunningServer server(config);
  auto client = server.client();
  EXPECT_EQ(client.put("key", std::string(65, 'x')).status,
            cache::protocol::Status::kValueTooLarge);
}
