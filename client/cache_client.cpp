#include "cache_client.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>

namespace cache {
namespace {

std::string errno_message(std::string_view what) {
  const int saved_errno = errno;
  return std::string(what) + ": " +
         std::error_code(saved_errno, std::generic_category()).message();
}

}  // namespace

CacheClient::CacheClient(std::string socket_path) : socket_path_(std::move(socket_path)) {}

CacheClient::~CacheClient() = default;

CacheClient::CacheClient(CacheClient&&) noexcept = default;
CacheClient& CacheClient::operator=(CacheClient&&) noexcept = default;

std::string CacheClient::connect() {
  if (socket_path_.size() >= sizeof(sockaddr_un::sun_path)) {
    return "socket path too long for AF_UNIX";
  }
  UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!fd) {
    return errno_message("socket");
  }

  if (timeout_ms_ > 0) {
    timeval tv{};
    tv.tv_sec = timeout_ms_ / 1000;
    tv.tv_usec = static_cast<suseconds_t>((timeout_ms_ % 1000) * 1000);
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
        ::setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
      return errno_message("setsockopt(SO_*TIMEO)");
    }
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, socket_path_.c_str(), socket_path_.size());
  if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    return errno_message("connect(" + socket_path_ + ")");
  }

  fd_ = std::move(fd);
  in_buf_.clear();
  return {};
}

void CacheClient::disconnect() {
  fd_.reset();
  in_buf_.clear();
}

std::string CacheClient::send_all(const serializer::Bytes& frame) {
  size_t sent = 0;
  while (sent < frame.size()) {
    // MSG_NOSIGNAL keeps a dead server from killing the client process.
    const ssize_t n =
        ::send(fd_.get(), frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return "send timed out";
    }
    return errno_message("send");
  }
  return {};
}

std::string CacheClient::receive_response(uint64_t expected_id, protocol::Opcode expected_opcode,
                                          protocol::Response& out) {
  constexpr protocol::Limits kClientLimits{protocol::kMaxKeyBytes, protocol::kMaxValueBytes};
  for (;;) {
    // Try to parse before reading: a previous read may already have delivered
    // the whole frame (or several).
    const protocol::ParseResult result =
        serializer::deserialize_response(serializer::ByteView(in_buf_), kClientLimits, out);
    if (result.status == protocol::ParseStatus::kComplete) {
      in_buf_.erase(in_buf_.begin(), in_buf_.begin() + static_cast<ptrdiff_t>(result.consumed));
      if (out.request_id != expected_id) {
        return "response correlation failure: expected request id " +
               std::to_string(expected_id) + ", got " + std::to_string(out.request_id);
      }
      if (out.opcode != expected_opcode) {
        return "response opcode mismatch: expected " +
               std::string(protocol::to_string(expected_opcode)) + ", got " +
               protocol::to_string(out.opcode);
      }
      return {};
    }
    if (result.status == protocol::ParseStatus::kMalformed) {
      disconnect();
      return std::string("malformed response frame: ") + protocol::to_string(result.error);
    }

    uint8_t chunk[16 * 1024];
    const ssize_t n = ::recv(fd_.get(), chunk, sizeof(chunk), 0);
    if (n > 0) {
      in_buf_.insert(in_buf_.end(), chunk, chunk + n);
      continue;
    }
    if (n == 0) {
      disconnect();
      return "server closed the connection";
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      disconnect();  // partial frame left in the buffer; the stream is unusable
      return "receive timed out";
    }
    const std::string message = errno_message("recv");
    disconnect();
    return message;
  }
}

CacheResult CacheClient::execute(protocol::Opcode opcode, std::string_view key,
                                 std::string_view value) {
  CacheResult result;
  if (!connected()) {
    result.error = "not connected";
    return result;
  }
  if (key.size() > protocol::kMaxKeyBytes) {
    result.error = "key exceeds protocol maximum";
    return result;
  }
  if (value.size() > protocol::kMaxValueBytes) {
    result.error = "value exceeds protocol maximum";
    return result;
  }

  protocol::Request request;
  request.opcode = opcode;
  request.request_id = next_request_id_++;
  request.key.assign(key);
  request.value.assign(value);

  out_buf_.clear();
  serializer::serialize_request_into(request, out_buf_);
  if (std::string error = send_all(out_buf_); !error.empty()) {
    result.error = std::move(error);
    return result;
  }

  protocol::Response response;
  if (std::string error = receive_response(request.request_id, request.opcode, response);
      !error.empty()) {
    result.error = std::move(error);
    return result;
  }
  result.status = response.status;
  result.value = std::move(response.value);
  return result;
}

CacheResult CacheClient::get(std::string_view key) {
  return execute(protocol::Opcode::kGet, key, {});
}

CacheResult CacheClient::put(std::string_view key, std::string_view value) {
  return execute(protocol::Opcode::kPut, key, value);
}

CacheResult CacheClient::erase(std::string_view key) {
  return execute(protocol::Opcode::kDelete, key, {});
}

CacheResult CacheClient::ping() { return execute(protocol::Opcode::kPing, {}, {}); }

CacheResult CacheClient::clear() { return execute(protocol::Opcode::kClear, {}, {}); }

StatsResult CacheClient::stats() {
  StatsResult result;
  CacheResult raw = execute(protocol::Opcode::kStats, {}, {});
  if (!raw.ok()) {
    result.error = std::move(raw.error);
    return result;
  }
  if (raw.status != protocol::Status::kOk) {
    result.error = std::string("server returned ") + protocol::to_string(raw.status);
    return result;
  }
  const serializer::ByteView view(reinterpret_cast<const uint8_t*>(raw.value.data()),
                                  raw.value.size());
  if (!serializer::deserialize_stats(view, result.stats)) {
    result.error = "malformed STATS payload";
  }
  return result;
}

}  // namespace cache
