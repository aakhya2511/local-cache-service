#include <gtest/gtest.h>

#include "cache/serializer.h"

namespace p = cache::protocol;
namespace s = cache::serializer;

TEST(SerializationTest, PrimitiveIntegersUseBigEndian) {
  s::Bytes bytes;
  s::append_u16(bytes, 0x1234); s::append_u32(bytes, 0x89abcdef); s::append_u64(bytes, 0x0102030405060708ULL);
  EXPECT_EQ(bytes[0], 0x12); EXPECT_EQ(bytes[1], 0x34);
  EXPECT_EQ(s::read_u32(bytes.data() + 2), 0x89abcdefU);
  EXPECT_EQ(s::read_u64(bytes.data() + 6), 0x0102030405060708ULL);
}

TEST(SerializationTest, RequestAndResponseRoundTripArbitraryBytes) {
  p::Request request{p::Opcode::kPut, 0x1020304050607080ULL,
                     std::string("k\0y", 3), std::string("\0\xff\x01", 3)};
  p::Request decoded;
  const auto encoded = s::serialize_request(request);
  const auto result = s::deserialize_request(encoded, {}, decoded);
  ASSERT_EQ(result.status, p::ParseStatus::kComplete);
  EXPECT_EQ(decoded.key, request.key); EXPECT_EQ(decoded.value, request.value);
  EXPECT_EQ(decoded.request_id, request.request_id);

  p::Response response{p::Opcode::kGet, p::Status::kOk, 9, request.value};
  p::Response decoded_response;
  EXPECT_EQ(s::deserialize_response(s::serialize_response(response), {}, decoded_response).status,
            p::ParseStatus::kComplete);
  EXPECT_EQ(decoded_response.value, response.value);
}

TEST(SerializationTest, StatsRoundTrip) {
  cache::StatsSnapshot input; input.cache_hits = 12; input.evictions = 8;
  input.current_cache_bytes = 999; input.uptime_ms = 1234;
  cache::StatsSnapshot output;
  const auto bytes = s::serialize_stats(input);
  ASSERT_TRUE(s::deserialize_stats(bytes, output));
  EXPECT_EQ(output.cache_hits, 12U); EXPECT_EQ(output.evictions, 8U);
  EXPECT_EQ(output.current_cache_bytes, 999U); EXPECT_EQ(output.uptime_ms, 1234U);
  EXPECT_FALSE(s::deserialize_stats(s::ByteView(bytes.data(), 1), output));
}
