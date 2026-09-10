#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cache/protocol.h"
#include "cache/serializer.h"
#include "cache/stats.h"
#include "cache/unique_fd.h"

namespace cache {

// Result of one synchronous cache operation.
//
// Two failure axes are kept separate on purpose:
//   * transport/protocol failure  -> ok() == false, `error` explains why
//   * server-side outcome         -> ok() == true, inspect `status`
// A missing key is not an error; it is Status::kNotFound.
struct CacheResult {
  protocol::Status status = protocol::Status::kInternalError;
  std::string value;
  std::string error;

  [[nodiscard]] bool ok() const noexcept { return error.empty(); }
  [[nodiscard]] bool found() const noexcept { return ok() && status == protocol::Status::kOk; }
};

struct StatsResult {
  StatsSnapshot stats;
  std::string error;
  [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Blocking, single-threaded client for the local cache service.
//
// One request is outstanding at a time; the socket is owned via RAII and
// closed on destruction. Not thread-safe: give each thread its own client
// (that is also what keeps request/response correlation trivial here).
class CacheClient {
 public:
  explicit CacheClient(std::string socket_path);
  ~CacheClient();

  CacheClient(const CacheClient&) = delete;
  CacheClient& operator=(const CacheClient&) = delete;
  CacheClient(CacheClient&&) noexcept;
  CacheClient& operator=(CacheClient&&) noexcept;

  // Empty string on success, otherwise an error description.
  [[nodiscard]] std::string connect();
  void disconnect();
  [[nodiscard]] bool connected() const noexcept { return fd_.valid(); }

  // Bounds blocking send/recv so a wedged server cannot hang a client forever.
  void set_timeout_ms(int timeout_ms) noexcept { timeout_ms_ = timeout_ms; }

  [[nodiscard]] CacheResult get(std::string_view key);
  [[nodiscard]] CacheResult put(std::string_view key, std::string_view value);
  [[nodiscard]] CacheResult erase(std::string_view key);
  [[nodiscard]] CacheResult ping();
  [[nodiscard]] CacheResult clear();
  [[nodiscard]] StatsResult stats();

 private:
  [[nodiscard]] CacheResult execute(protocol::Opcode opcode, std::string_view key,
                                    std::string_view value);
  [[nodiscard]] std::string send_all(const serializer::Bytes& frame);
  [[nodiscard]] std::string receive_response(uint64_t expected_id, protocol::Opcode expected_opcode,
                                             protocol::Response& out);

  std::string socket_path_;
  UniqueFd fd_;
  uint64_t next_request_id_ = 1;
  int timeout_ms_ = 5000;
  std::vector<uint8_t> in_buf_;
  serializer::Bytes out_buf_;
};

}  // namespace cache
