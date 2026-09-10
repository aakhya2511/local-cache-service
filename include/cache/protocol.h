#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cache::protocol {

// ---------------------------------------------------------------------------
// Wire format (all multi-byte integers are big endian / network byte order).
//
//   offset  size  field
//   ------  ----  -----------------------------------------------------------
//        0     4  magic        0x4C435331 ("LCS1")
//        4     1  version      current = 1
//        5     1  opcode       request: command; response: echoed command
//        6     1  flags        reserved, must be 0 in v1
//        7     1  status       request: must be 0; response: Status
//        8     8  request_id   client-chosen correlation id
//       16     4  key_len      bytes of key that follow the header
//       20     4  value_len    bytes of value that follow the key
//       24   ...  payload      key bytes, then value bytes
//
// Header is a fixed 24 bytes. Every field is encoded explicitly; no struct is
// ever cast onto the wire (padding/alignment/endianness/ABI are not portable).
// ---------------------------------------------------------------------------

inline constexpr uint32_t kMagic = 0x4C435331U;  // "LCS1"
inline constexpr uint8_t kVersion = 1;
inline constexpr size_t kHeaderSize = 24;

// Absolute protocol ceilings. A server may configure smaller limits, never
// larger; these bound what the parser will ever consider a legal frame.
inline constexpr uint32_t kMaxKeyBytes = 64U * 1024;
inline constexpr uint32_t kMaxValueBytes = 64U * 1024 * 1024;
inline constexpr size_t kMaxFrameBytes =
    kHeaderSize + static_cast<size_t>(kMaxKeyBytes) + static_cast<size_t>(kMaxValueBytes);

enum class Opcode : uint8_t {
  kGet = 1,
  kPut = 2,
  kDelete = 3,
  kPing = 4,
  kStats = 5,
  kClear = 6,
};

enum class Status : uint8_t {
  kOk = 0,
  kNotFound = 1,
  kInvalidRequest = 2,
  kValueTooLarge = 3,
  kServerBusy = 4,
  kInternalError = 5,
  kUnsupportedVersion = 6,
};

[[nodiscard]] bool is_valid_opcode(uint8_t raw) noexcept;
[[nodiscard]] bool is_valid_status(uint8_t raw) noexcept;
[[nodiscard]] const char* to_string(Opcode opcode) noexcept;
[[nodiscard]] const char* to_string(Status status) noexcept;

struct Header {
  uint8_t version = kVersion;
  uint8_t opcode = 0;
  uint8_t flags = 0;
  uint8_t status = 0;
  uint64_t request_id = 0;
  uint32_t key_len = 0;
  uint32_t value_len = 0;
};

struct Request {
  Opcode opcode = Opcode::kPing;
  uint64_t request_id = 0;
  std::string key;
  std::string value;  // binary-safe; may contain NUL bytes
};

struct Response {
  Opcode opcode = Opcode::kPing;
  Status status = Status::kOk;
  uint64_t request_id = 0;
  std::string value;  // binary-safe payload (GET result, STATS blob, ...)
};

enum class ParseStatus : uint8_t {
  kComplete,
  kNeedMoreData,
  kMalformed,
};

// Why a frame was rejected. Reported in logs and used by tests to assert that
// defensive checks fire for the right reason rather than by accident.
enum class ParseError : uint8_t {
  kNone = 0,
  kBadMagic,
  kUnsupportedVersion,
  kInvalidOpcode,
  kInvalidStatus,
  kReservedFlags,
  kKeyTooLarge,
  kValueTooLarge,
  kPayloadNotAllowed,  // e.g. a value on a GET, or a key on a response
  kMissingKey,
};

[[nodiscard]] const char* to_string(ParseError error) noexcept;

struct ParseResult {
  ParseStatus status = ParseStatus::kNeedMoreData;
  ParseError error = ParseError::kNone;
  size_t consumed = 0;  // bytes consumed from the input buffer when kComplete

  static ParseResult complete(size_t consumed) {
    return ParseResult{ParseStatus::kComplete, ParseError::kNone, consumed};
  }
  static ParseResult need_more() { return ParseResult{}; }
  static ParseResult malformed(ParseError error) {
    return ParseResult{ParseStatus::kMalformed, error, 0};
  }
};

// Per-parse limits, normally derived from ServerConfig. Values above the
// protocol ceilings are clamped by the parser.
struct Limits {
  uint32_t max_key_bytes = kMaxKeyBytes;
  uint32_t max_value_bytes = kMaxValueBytes;
};

// Whether a request opcode is allowed to carry a key / value payload.
[[nodiscard]] bool opcode_requires_key(Opcode opcode) noexcept;
[[nodiscard]] bool opcode_allows_value(Opcode opcode) noexcept;

}  // namespace cache::protocol
