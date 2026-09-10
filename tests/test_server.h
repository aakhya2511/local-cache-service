#pragma once

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "cache/server.h"
#include "cache_client.h"

class RunningServer {
 public:
  explicit RunningServer(cache::ServerConfig config = {})
      : path_("/tmp/lcs_test_" + std::to_string(::getpid()) + "_" +
              std::to_string(next_.fetch_add(1)) + ".sock") {
    config.socket_path = path_;
    config.worker_threads = 4;
    config.max_connections = 2048;
    server_ = std::make_unique<cache::Server>(config);
    std::string error;
    if (!server_->start(&error)) throw std::runtime_error(error);
    thread_ = std::thread([this] { server_->run(); });
  }

  ~RunningServer() { stop(); }
  RunningServer(const RunningServer&) = delete;
  RunningServer& operator=(const RunningServer&) = delete;

  void stop() {
    if (!server_) return;
    server_->request_shutdown();
    if (thread_.joinable()) thread_.join();
    server_.reset();
  }
  const std::string& path() const { return path_; }
  cache::Server& server() { return *server_; }

  cache::CacheClient client() const {
    cache::CacheClient client(path_);
    const std::string error = client.connect();
    if (!error.empty()) throw std::runtime_error(error);
    return client;
  }

 private:
  inline static std::atomic<unsigned> next_{0};
  std::string path_;
  std::unique_ptr<cache::Server> server_;
  std::thread thread_;
};
