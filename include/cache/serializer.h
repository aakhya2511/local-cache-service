#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cache/protocol.h"
#include "cache/stats.h"

namespace cache::serializer {

using Bytes = std::vector<uint8_t>;
using ByteView = std::span<const uint8_t>;

// --- primitive big-endian encoding -----------------------------------------
void append_u8(Bytes& out, uint8_t value);
void append_u16(Bytes& out, uint16_t value);
void append_u32(Bytes& out, uint32_t value);
void append_u64(Bytes& out, uint64_t value);

[[nodiscard]] uint8_t read_u8(const uint8_t* data) noexcept;
[[nodiscard]] uint16_t read_u16(const uint8_t* data) noexcept;
[[nodiscard]] uint32_t read_u32(const uint8_t* data) noexcept;
[[nodiscard]] uint64_t read_u64(const uint8_t* data) noexcept;

// --- frames -----------------------------------------------------------------
void serialize_request_into(const protocol::Request& request, Bytes& out);
[[nodiscard]] Bytes serialize_request(const protocol::Request& request);

void serialize_response_into(const protocol::Response& response, Bytes& out);
[[nodiscard]] Bytes serialize_response(const protocol::Response& response);

// Incremental parsers. `input` is the contiguous head of a stream; the parser
// never assumes it holds exactly one frame.
//
//   kComplete      -> `out` populated, result.consumed bytes may be dropped
//   kNeedMoreData  -> nothing consumed, call again with more bytes
//   kMalformed     -> stream cannot be resynchronized; caller must close
//
// Length fields are validated against `limits` *before* any allocation.
[[nodiscard]] protocol::ParseResult deserialize_request(ByteView input, protocol::Limits limits,
                                                        protocol::Request& out);

[[nodiscard]] protocol::ParseResult deserialize_response(ByteView input, protocol::Limits limits,
                                                         protocol::Response& out);

// --- STATS payload ----------------------------------------------------------
// Encoded as: u16 field_count, then field_count big-endian u64 values in the
// declaration order of StatsSnapshot. Readers accept a different count so an
// older client can still talk to a newer server.
[[nodiscard]] Bytes serialize_stats(const StatsSnapshot& stats);
[[nodiscard]] bool deserialize_stats(ByteView input, StatsSnapshot& out);

}  // namespace cache::serializer
