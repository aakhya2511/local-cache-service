#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "async_client.h"

namespace {
struct Options { std::string socket = "/tmp/local_cache.sock"; size_t connections = 500;
  size_t pipeline = 32; uint64_t requests = 100'000; };

std::string payload(size_t connection, uint64_t sequence) {
  std::string value = "LCS-STRESS:" + std::to_string(connection) + ":" + std::to_string(sequence) + ":";
  uint64_t state = (sequence + 1) * 0x9e3779b97f4a7c15ULL ^ connection;
  while (value.size() < 256) { state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
    value.push_back(static_cast<char>(state & 0xff)); }
  return value;
}
bool parse_number(std::string_view s, uint64_t& out) {
  char* end = nullptr; const std::string copy(s); out = std::strtoull(copy.c_str(), &end, 10);
  return !s.empty() && *end == '\0' && out > 0;
}
}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (i + 1 >= argc) { std::fprintf(stderr, "missing option value\n"); return 2; }
    const std::string_view value(argv[++i]);
    if (arg == "--socket") o.socket = value;
    else { uint64_t n = 0; if (!parse_number(value, n)) return 2;
      if (arg == "--connections") o.connections = static_cast<size_t>(n);
      else if (arg == "--pipeline") o.pipeline = static_cast<size_t>(n);
      else if (arg == "--requests") o.requests = n;
      else return 2; }
  }
  uint64_t validation_failures = 0, protocol_failures = 0, corruption_failures = 0;
  bench::Engine put_engine(
      bench::EngineConfig{o.socket, o.connections, o.pipeline, o.requests},
      [&](size_t, uint64_t sequence, bench::RequestSpec& request) {
        request.opcode = cache::protocol::Opcode::kPut;
        request.key = "stress:" + std::to_string(sequence);
        request.value = payload(0, sequence);
        return true;
      },
      [&](const bench::Completion& completion) {
        if (completion.response.request_id == 0) ++protocol_failures;
        if (completion.response.status != cache::protocol::Status::kOk) ++validation_failures;
        if (!completion.response.value.empty()) ++corruption_failures;
      });
  std::string error = put_engine.connect_all();
  if (error.empty()) error = put_engine.run();

  bench::Engine get_engine(
      bench::EngineConfig{o.socket, o.connections, o.pipeline, o.requests},
      [&](size_t, uint64_t sequence, bench::RequestSpec& request) {
        request.opcode = cache::protocol::Opcode::kGet;
        request.key = "stress:" + std::to_string(sequence);
        return true;
      },
      [&](const bench::Completion& completion) {
        if (completion.response.request_id == 0) ++protocol_failures;
        if (completion.response.status != cache::protocol::Status::kOk) {
          ++validation_failures;
          return;
        }
        const auto separator = completion.request.key.rfind(':');
        const uint64_t sequence = std::strtoull(completion.request.key.c_str() + separator + 1,
                                                nullptr, 10);
        if (completion.response.value != payload(0, sequence)) ++corruption_failures;
      });
  if (error.empty()) error = get_engine.connect_all();
  if (error.empty()) error = get_engine.run();
  const auto& put_stats = put_engine.stats();
  const auto& get_stats = get_engine.stats();
  const uint64_t sent = put_stats.sent + get_stats.sent;
  const uint64_t received = put_stats.received + get_stats.received;
  const uint64_t connection_failures =
      put_stats.connect_failures + get_stats.connect_failures;
  const uint64_t io_failures = put_stats.io_failures + get_stats.io_failures;
  protocol_failures += put_stats.protocol_failures + get_stats.protocol_failures;
  std::printf("Stress Validation\n-----------------------------\nConnections:              %zu\n"
              "Pipeline depth:           %zu\nTarget in-flight:         %zu\n"
              "Requests sent:           %llu\nResponses received:      %llu\n"
              "Validation failures:     %llu\nProtocol failures:       %llu\n"
              "Connection failures:     %llu\nI/O failures:            %llu\n"
              "Data corruption failures: %llu\n",
              o.connections, o.pipeline, o.connections * o.pipeline,
              static_cast<unsigned long long>(sent),
              static_cast<unsigned long long>(received),
              static_cast<unsigned long long>(validation_failures),
              static_cast<unsigned long long>(protocol_failures),
              static_cast<unsigned long long>(connection_failures),
              static_cast<unsigned long long>(io_failures),
              static_cast<unsigned long long>(corruption_failures));
  if (!error.empty()) std::fprintf(stderr, "%s\n", error.c_str());
  return error.empty() && validation_failures == 0 && protocol_failures == 0 &&
                 connection_failures == 0 && io_failures == 0 && corruption_failures == 0 ? 0 : 1;
}
