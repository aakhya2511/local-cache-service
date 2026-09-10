#include <gtest/gtest.h>

#include "cache/serializer.h"

namespace p = cache::protocol;
namespace s = cache::serializer;

TEST(ProtocolTest, RejectsInvalidHeaderFieldsAndLengthsBeforePayload) {
  p::Request request{p::Opcode::kPut, 42, "key", "value"};
  auto frame = s::serialize_request(request);
  p::Request parsed;
  frame[0] = 0;
  EXPECT_EQ(s::deserialize_request(frame, {}, parsed).error, p::ParseError::kBadMagic);
  frame = s::serialize_request(request); frame[4] = 99;
  EXPECT_EQ(s::deserialize_request(frame, {}, parsed).error, p::ParseError::kUnsupportedVersion);
  frame = s::serialize_request(request); frame[5] = 99;
  EXPECT_EQ(s::deserialize_request(frame, {}, parsed).error, p::ParseError::kInvalidOpcode);
  frame = s::serialize_request(request); frame[6] = 1;
  EXPECT_EQ(s::deserialize_request(frame, {}, parsed).error, p::ParseError::kReservedFlags);
  frame = s::serialize_request(request); frame[16] = 0x7f;
  EXPECT_EQ(s::deserialize_request(frame, p::Limits{1024, 1024}, parsed).error,
            p::ParseError::kKeyTooLarge);
  frame = s::serialize_request(request); frame[20] = 0x7f;
  EXPECT_EQ(s::deserialize_request(frame, p::Limits{1024, 1024}, parsed).error,
            p::ParseError::kValueTooLarge);
}

TEST(ProtocolTest, DistinguishesTruncationFromMalformedData) {
  const auto frame = s::serialize_request(p::Request{p::Opcode::kPut, 7, "abc", "def"});
  p::Request parsed;
  for (size_t size = 0; size < frame.size(); ++size) {
    EXPECT_EQ(s::deserialize_request(s::ByteView(frame.data(), size), {}, parsed).status,
              p::ParseStatus::kNeedMoreData) << size;
  }
  EXPECT_EQ(s::deserialize_request(frame, {}, parsed).status, p::ParseStatus::kComplete);
}

TEST(ProtocolTest, ParsesFirstOfMultipleFrames) {
  auto both = s::serialize_request(p::Request{p::Opcode::kPing, 1, {}, {}});
  const auto second = s::serialize_request(p::Request{p::Opcode::kGet, 2, "k", {}});
  both.insert(both.end(), second.begin(), second.end());
  p::Request parsed;
  const auto first = s::deserialize_request(both, {}, parsed);
  ASSERT_EQ(first.status, p::ParseStatus::kComplete);
  EXPECT_EQ(first.consumed, p::kHeaderSize);
  EXPECT_EQ(parsed.request_id, 1U);
  const auto next = s::deserialize_request(s::ByteView(both).subspan(first.consumed), {}, parsed);
  EXPECT_EQ(next.status, p::ParseStatus::kComplete);
  EXPECT_EQ(parsed.request_id, 2U);
}
