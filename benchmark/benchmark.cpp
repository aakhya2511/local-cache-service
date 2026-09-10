#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "async_client.h"
#include "cache_client.h"

namespace {
struct Options {
  std::string socket = "/tmp/local_cache.sock";
  size_t clients = 100;
  size_t pipeline = 1;
  uint64_t requests = 100'000;
  size_t key_space = 10'000;
  unsigned read_percent = 95;
  unsigned put_percent = 5;
  size_t value_bytes = 128;
};

bool number(std::string_view text, uint64_t& value) {
  if (text.empty()) return false;
  char* end = nullptr;
  const std::string copy(text);
  const unsigned long long parsed = std::strtoull(copy.c_str(), &end, 10);
  if (*end != '\0') return false;
  value = static_cast<uint64_t>(parsed);
  return true;
}

void usage(const char* name) {
  std::printf("Usage: %s [--socket PATH] [--clients N] [--pipeline N] [--requests N] "
              "[--key-space N] [--read-percent N] [--put-percent N] [--value-bytes N]\n",
              name);
}

bool parse(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--help") { usage(argv[0]); std::exit(0); }
    if (i + 1 >= argc) return false;
    const std::string_view value(argv[++i]);
    if (arg == "--socket") { options.socket = value; continue; }
    uint64_t parsed = 0;
    if (!number(value, parsed)) return false;
    if (arg == "--clients") options.clients = static_cast<size_t>(parsed);
    else if (arg == "--pipeline") options.pipeline = static_cast<size_t>(parsed);
    else if (arg == "--requests") options.requests = parsed;
    else if (arg == "--key-space") options.key_space = static_cast<size_t>(parsed);
    else if (arg == "--read-percent") options.read_percent = static_cast<unsigned>(parsed);
    else if (arg == "--put-percent") options.put_percent = static_cast<unsigned>(parsed);
    else if (arg == "--value-bytes") options.value_bytes = static_cast<size_t>(parsed);
    else return false;
  }
  return options.clients > 0 && options.pipeline > 0 && options.requests > 0 &&
         options.key_space > 0 && options.read_percent + options.put_percent <= 100;
}

double percentile(std::vector<uint64_t>& values, double fraction) {
  if (values.empty()) return 0.0;
  const size_t index =
      static_cast<size_t>(std::ceil(fraction * static_cast<double>(values.size()))) - 1;
  return static_cast<double>(values[std::min(index, values.size() - 1)]) / 1e6;
}
}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse(argc, argv, options)) { usage(argv[0]); return 2; }

  cache::CacheClient setup(options.socket);
  if (const auto error = setup.connect(); !error.empty()) {
    std::fprintf(stderr, "setup connection failed: %s\n", error.c_str());
    return 1;
  }
  const std::string value(options.value_bytes, 'x');
  for (size_t i = 0; i < options.key_space; ++i) {
    const auto result = setup.put("bench:" + std::to_string(i), value);
    if (!result.ok() || result.status != cache::protocol::Status::kOk) {
      std::fprintf(stderr, "prepopulation failed at key %zu\n", i);
      return 1;
    }
  }

  uint64_t successful = 0;
  uint64_t failed = 0;
  uint64_t get_hits = 0;
  uint64_t gets = 0;
  std::vector<uint64_t> latencies;
  latencies.reserve(static_cast<size_t>(options.requests));
  std::mt19937_64 random(0x4c435331ULL);

  bench::Engine engine(
      bench::EngineConfig{options.socket, options.clients, options.pipeline, options.requests},
      [&](size_t, uint64_t, bench::RequestSpec& request) {
        const unsigned choice = static_cast<unsigned>(random() % 100);
        const size_t key = static_cast<size_t>(random() % options.key_space);
        request.key = "bench:" + std::to_string(key);
        if (choice < options.read_percent) request.opcode = cache::protocol::Opcode::kGet;
        else if (choice < options.read_percent + options.put_percent) {
          request.opcode = cache::protocol::Opcode::kPut;
          request.value = value;
        } else request.opcode = cache::protocol::Opcode::kDelete;
        return true;
      },
      [&](const bench::Completion& completion) {
        latencies.push_back(completion.latency_ns);
        const bool accepted = completion.response.status == cache::protocol::Status::kOk ||
                              completion.response.status == cache::protocol::Status::kNotFound;
        accepted ? ++successful : ++failed;
        if (completion.request.opcode == cache::protocol::Opcode::kGet) {
          ++gets;
          if (completion.response.status == cache::protocol::Status::kOk) ++get_hits;
        }
      });
  if (const auto error = engine.connect_all(); !error.empty()) {
    std::fprintf(stderr, "connect failed: %s\n", error.c_str()); return 1;
  }
  const auto begin = std::chrono::steady_clock::now();
  if (const auto error = engine.run(); !error.empty()) {
    std::fprintf(stderr, "benchmark failed: %s\n", error.c_str()); return 1;
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
  std::sort(latencies.begin(), latencies.end());
  long double sum = 0;
  for (const uint64_t latency : latencies) sum += latency;
  const double average = latencies.empty() ? 0.0 : static_cast<double>(sum / latencies.size()) / 1e6;
  const double maximum = latencies.empty() ? 0.0 : static_cast<double>(latencies.back()) / 1e6;

  std::printf("Benchmark Results\n-----------------------------\n"
              "Clients:             %zu\nPipeline depth:       %zu\nRequests:            %llu\n"
              "Successful:          %llu\nFailed:              %llu\nDuration:            %.3f s\n"
              "Throughput:          %.0f req/s\nGET hit rate:        %.2f%%\n\n"
              "Latency\n-----------------------------\nAverage:             %.3f ms\n"
              "P50:                 %.3f ms\nP95:                 %.3f ms\n"
              "P99:                 %.3f ms\nMax:                 %.3f ms\n",
              options.clients, options.pipeline, static_cast<unsigned long long>(options.requests),
              static_cast<unsigned long long>(successful), static_cast<unsigned long long>(failed),
              seconds, static_cast<double>(options.requests) / seconds,
              gets == 0 ? 0.0 : 100.0 * static_cast<double>(get_hits) / static_cast<double>(gets),
              average, percentile(latencies, .50), percentile(latencies, .95),
              percentile(latencies, .99), maximum);
  return failed == 0 ? 0 : 1;
}
