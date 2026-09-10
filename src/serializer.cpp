#include "cache/serializer.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace cache::serializer {
namespace {

using protocol::ParseError;
using protocol::ParseResult;

// Single source of truth for the STATS field order on the wire.
constexpr std::array<uint64_t StatsSnapshot::*, 14> kStatsFields = {
    &StatsSnapshot::cache_hits,           &StatsSnapshot::cache_misses,
    &StatsSnapshot::puts,                 &StatsSnapshot::deletes,
    &StatsSnapshot::evictions,            &StatsSnapshot::current_entries,
    &StatsSnapshot::current_cache_bytes,  &StatsSnapshot::current_payload_bytes,
    &StatsSnapshot::active_connections,   &StatsSnapshot::total_connections,
    &StatsSnapshot::requests_processed,   &StatsSnapshot::protocol_errors,
    &StatsSnapshot::rejected_requests,    &StatsSnapshot::uptime_ms,
};

void append_bytes(Bytes& out, std::string_view data) {
  const auto* first = reinterpret_cast<const uint8_t*>(data.data());
  out.insert(out.end(), first, first + data.size());
}

void encode_header(Bytes& out, const protocol::Header& header) {
  append_u32(out, protocol::kMagic);
  append_u8(out, header.version);
  append_u8(out, header.opcode);
  append_u8(out, header.flags);
  append_u8(out, header.status);
  append_u64(out, header.request_id);
  append_u32(out, header.key_len);
  append_u32(out, header.value_len);
}

// Decodes and validates the fixed part shared by requests and responses.
// Returns kNone when the header is structurally acceptable.
ParseError decode_common_header(const uint8_t* data, protocol::Header& header) {
  if (read_u32(data) != protocol::kMagic) {
    return ParseError::kBadMagic;
  }
  header.version = read_u8(data + 4);
  if (header.version != protocol::kVersion) {
    return ParseError::kUnsupportedVersion;
  }
  header.opcode = read_u8(data + 5);
  if (!protocol::is_valid_opcode(header.opcode)) {
    return ParseError::kInvalidOpcode;
  }
  header.flags = read_u8(data + 6);
  if (header.flags != 0) {
    return ParseError::kReservedFlags;
  }
  header.status = read_u8(data + 7);
  header.request_id = read_u64(data + 8);
  header.key_len = read_u32(data + 16);
  header.value_len = read_u32(data + 20);
  return ParseError::kNone;
}

uint32_t effective_limit(uint32_t configured, uint32_t ceiling) {
  return std::min(configured, ceiling);
}

}  // namespace

void append_u8(Bytes& out, uint8_t value) { out.push_back(value); }

void append_u16(Bytes& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void append_u32(Bytes& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void append_u64(Bytes& out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(value >> shift));
  }
}

uint8_t read_u8(const uint8_t* data) noexcept { return data[0]; }

uint16_t read_u16(const uint8_t* data) noexcept {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

uint32_t read_u32(const uint8_t* data) noexcept {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

uint64_t read_u64(const uint8_t* data) noexcept {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<uint64_t>(data[i]);
  }
  return value;
}

void serialize_request_into(const protocol::Request& request, Bytes& out) {
  protocol::Header header;
  header.opcode = static_cast<uint8_t>(request.opcode);
  header.request_id = request.request_id;
  header.key_len = static_cast<uint32_t>(request.key.size());
  header.value_len = static_cast<uint32_t>(request.value.size());
  out.reserve(out.size() + protocol::kHeaderSize + request.key.size() + request.value.size());
  encode_header(out, header);
  append_bytes(out, request.key);
  append_bytes(out, request.value);
}

Bytes serialize_request(const protocol::Request& request) {
  Bytes out;
  serialize_request_into(request, out);
  return out;
}

void serialize_response_into(const protocol::Response& response, Bytes& out) {
  protocol::Header header;
  header.opcode = static_cast<uint8_t>(response.opcode);
  header.status = static_cast<uint8_t>(response.status);
  header.request_id = response.request_id;
  header.key_len = 0;
  header.value_len = static_cast<uint32_t>(response.value.size());
  out.reserve(out.size() + protocol::kHeaderSize + response.value.size());
  encode_header(out, header);
  append_bytes(out, response.value);
}

Bytes serialize_response(const protocol::Response& response) {
  Bytes out;
  serialize_response_into(response, out);
  return out;
}

ParseResult deserialize_request(ByteView input, protocol::Limits limits, protocol::Request& out) {
  if (input.size() < protocol::kHeaderSize) {
    return ParseResult::need_more();
  }

  protocol::Header header;
  if (const ParseError error = decode_common_header(input.data(), header);
      error != ParseError::kNone) {
    return ParseResult::malformed(error);
  }
  if (header.status != 0) {
    return ParseResult::malformed(ParseError::kInvalidStatus);
  }

  const auto opcode = static_cast<protocol::Opcode>(header.opcode);
  if (header.key_len > effective_limit(limits.max_key_bytes, protocol::kMaxKeyBytes)) {
    return ParseResult::malformed(ParseError::kKeyTooLarge);
  }
  if (header.value_len > effective_limit(limits.max_value_bytes, protocol::kMaxValueBytes)) {
    return ParseResult::malformed(ParseError::kValueTooLarge);
  }
  if (protocol::opcode_requires_key(opcode) && header.key_len == 0) {
    return ParseResult::malformed(ParseError::kMissingKey);
  }
  if (!protocol::opcode_requires_key(opcode) && header.key_len != 0) {
    return ParseResult::malformed(ParseError::kPayloadNotAllowed);
  }
  if (!protocol::opcode_allows_value(opcode) && header.value_len != 0) {
    return ParseResult::malformed(ParseError::kPayloadNotAllowed);
  }

  // Both lengths are uint32_t and already bounded, so this cannot overflow a
  // 64-bit size_t. The frame total is still re-checked against kMaxFrameBytes.
  const size_t frame_size =
      protocol::kHeaderSize + static_cast<size_t>(header.key_len) + header.value_len;
  static_assert(sizeof(size_t) >= 8, "frame size arithmetic assumes 64-bit size_t");
  if (frame_size > protocol::kMaxFrameBytes) {
    return ParseResult::malformed(ParseError::kValueTooLarge);
  }
  if (input.size() < frame_size) {
    return ParseResult::need_more();  // no allocation performed yet
  }

  const auto* payload = reinterpret_cast<const char*>(input.data() + protocol::kHeaderSize);
  out.opcode = opcode;
  out.request_id = header.request_id;
  out.key.assign(payload, header.key_len);
  out.value.assign(payload + header.key_len, header.value_len);
  return ParseResult::complete(frame_size);
}

ParseResult deserialize_response(ByteView input, protocol::Limits limits, protocol::Response& out) {
  if (input.size() < protocol::kHeaderSize) {
    return ParseResult::need_more();
  }

  protocol::Header header;
  if (const ParseError error = decode_common_header(input.data(), header);
      error != ParseError::kNone) {
    return ParseResult::malformed(error);
  }
  if (!protocol::is_valid_status(header.status)) {
    return ParseResult::malformed(ParseError::kInvalidStatus);
  }
  if (header.key_len != 0) {
    return ParseResult::malformed(ParseError::kPayloadNotAllowed);
  }
  if (header.value_len > effective_limit(limits.max_value_bytes, protocol::kMaxValueBytes)) {
    return ParseResult::malformed(ParseError::kValueTooLarge);
  }

  const size_t frame_size = protocol::kHeaderSize + header.value_len;
  if (input.size() < frame_size) {
    return ParseResult::need_more();
  }

  out.opcode = static_cast<protocol::Opcode>(header.opcode);
  out.status = static_cast<protocol::Status>(header.status);
  out.request_id = header.request_id;
  out.value.assign(reinterpret_cast<const char*>(input.data() + protocol::kHeaderSize),
                   header.value_len);
  return ParseResult::complete(frame_size);
}

Bytes serialize_stats(const StatsSnapshot& stats) {
  Bytes out;
  out.reserve(2 + kStatsFields.size() * 8);
  append_u16(out, static_cast<uint16_t>(kStatsFields.size()));
  for (const auto field : kStatsFields) {
    append_u64(out, stats.*field);
  }
  return out;
}

bool deserialize_stats(ByteView input, StatsSnapshot& out) {
  if (input.size() < 2) {
    return false;
  }
  const uint16_t count = read_u16(input.data());
  if (input.size() < 2 + static_cast<size_t>(count) * 8) {
    return false;
  }
  const size_t readable = std::min<size_t>(count, kStatsFields.size());
  for (size_t i = 0; i < readable; ++i) {
    out.*kStatsFields[i] = read_u64(input.data() + 2 + i * 8);
  }
  return true;
}

}  // namespace cache::serializer
