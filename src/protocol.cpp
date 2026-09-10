#include "cache/protocol.h"

namespace cache::protocol {

bool is_valid_opcode(uint8_t raw) noexcept {
  switch (static_cast<Opcode>(raw)) {
    case Opcode::kGet:
    case Opcode::kPut:
    case Opcode::kDelete:
    case Opcode::kPing:
    case Opcode::kStats:
    case Opcode::kClear:
      return true;
  }
  return false;
}

bool is_valid_status(uint8_t raw) noexcept {
  switch (static_cast<Status>(raw)) {
    case Status::kOk:
    case Status::kNotFound:
    case Status::kInvalidRequest:
    case Status::kValueTooLarge:
    case Status::kServerBusy:
    case Status::kInternalError:
    case Status::kUnsupportedVersion:
      return true;
  }
  return false;
}

const char* to_string(Opcode opcode) noexcept {
  switch (opcode) {
    case Opcode::kGet: return "GET";
    case Opcode::kPut: return "PUT";
    case Opcode::kDelete: return "DELETE";
    case Opcode::kPing: return "PING";
    case Opcode::kStats: return "STATS";
    case Opcode::kClear: return "CLEAR";
  }
  return "UNKNOWN";
}

const char* to_string(Status status) noexcept {
  switch (status) {
    case Status::kOk: return "OK";
    case Status::kNotFound: return "NOT_FOUND";
    case Status::kInvalidRequest: return "INVALID_REQUEST";
    case Status::kValueTooLarge: return "VALUE_TOO_LARGE";
    case Status::kServerBusy: return "SERVER_BUSY";
    case Status::kInternalError: return "INTERNAL_ERROR";
    case Status::kUnsupportedVersion: return "UNSUPPORTED_VERSION";
  }
  return "UNKNOWN_STATUS";
}

const char* to_string(ParseError error) noexcept {
  switch (error) {
    case ParseError::kNone: return "none";
    case ParseError::kBadMagic: return "bad magic";
    case ParseError::kUnsupportedVersion: return "unsupported version";
    case ParseError::kInvalidOpcode: return "invalid opcode";
    case ParseError::kInvalidStatus: return "invalid status";
    case ParseError::kReservedFlags: return "reserved flags set";
    case ParseError::kKeyTooLarge: return "key too large";
    case ParseError::kValueTooLarge: return "value too large";
    case ParseError::kPayloadNotAllowed: return "payload not allowed for opcode";
    case ParseError::kMissingKey: return "missing key";
  }
  return "unknown";
}

bool opcode_requires_key(Opcode opcode) noexcept {
  switch (opcode) {
    case Opcode::kGet:
    case Opcode::kPut:
    case Opcode::kDelete:
      return true;
    case Opcode::kPing:
    case Opcode::kStats:
    case Opcode::kClear:
      return false;
  }
  return false;
}

bool opcode_allows_value(Opcode opcode) noexcept {
  // Only PUT carries a value. Rejecting stray payloads keeps the parser strict
  // and prevents a client from making the server buffer bytes it will ignore.
  return opcode == Opcode::kPut;
}

}  // namespace cache::protocol
