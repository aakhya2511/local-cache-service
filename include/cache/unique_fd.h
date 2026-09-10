#pragma once

#include <unistd.h>

#include <utility>

namespace cache {

// Move-only owner of a POSIX file descriptor.
//
// Descriptor leaks are the most common resource bug in socket servers, so every
// descriptor in this project (listening socket, accepted connection, epoll,
// eventfd, signalfd) is owned by one of these.
class UniqueFd {
 public:
  UniqueFd() noexcept = default;
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.fd_, -1));
    }
    return *this;
  }

  ~UniqueFd() { reset(); }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
  explicit operator bool() const noexcept { return valid(); }

  void reset(int fd = -1) noexcept {
    if (fd_ >= 0 && fd_ != fd) {
      // close() can fail (EINTR on some systems), but retrying is unsafe on
      // Linux: the descriptor is already released. Nothing useful to do here.
      ::close(fd_);
    }
    fd_ = fd;
  }

  [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

 private:
  int fd_ = -1;
};

}  // namespace cache
