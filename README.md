# Local Cache Service

A bounded, machine-local in-memory LRU cache service for Linux, implemented in C++20. Clients use a
versioned binary protocol over a Unix domain stream socket; one level-triggered `epoll` loop
multiplexes connections and a fixed worker pool performs cache operations.

Measured in a Release build on an Ubuntu 24.04 ARM64 Docker environment: 100 clients completed a
95% GET / 5% PUT workload at **0.980 ms average** and **101,905 requests/second**. A separate run
validated 200,000 responses with 500 connections × 32 outstanding requests and zero data-corruption
failures. These results are environment-specific; full methodology and tail latency are in
[docs/BENCHMARK_RESULTS.md](docs/BENCHMARK_RESULTS.md).

```text
client processes -> AF_UNIX stream -> epoll / connection buffers
                                      -> bounded work queue -> worker threads
                                                              -> mutex-protected LRU
                                      <- eventfd completions <- response serialization
```

Unix domain sockets fit a machine-local service: they avoid IP routing, have lower local IPC
overhead than TCP loopback, and use filesystem ownership/mode for access control. The daemon sets
the socket to owner-only (`0600`).

## Design at a glance

- O(1)-average `GET`, `PUT`, and `DELETE` use `unordered_map<string_view, list::iterator>` plus a
  doubly-linked `std::list`. The list front is MRU; successful GETs and updates promote their node.
- One cache mutex preserves exact global LRU order. No reference into cache storage escapes it.
- Entry count, maximum value size, and accounted cache bytes are independently bounded. Insertion
  repeatedly evicts from LRU until both capacity limits hold.
- The 24-byte protocol header is explicitly encoded in big-endian order. It contains magic,
  version, opcode, flags/status, request ID, and key/value lengths. Values are arbitrary bytes.
- Per-connection input/output state handles fragmentation, coalesced frames, partial writes, and
  `EAGAIN`. Per-connection in-flight/output limits and the bounded global queue provide backpressure.
- `signalfd` routes SIGINT/SIGTERM into the event loop; `eventfd` carries shutdown and worker
  completion notifications without unsafe signal-handler work.
- Pipelined requests are correlated by request ID but have no per-connection execution-order
  guarantee once dispatched to different workers.

See [docs/DESIGN.md](docs/DESIGN.md) for invariants, trade-offs, and the full wire specification.
The [interview guide](docs/INTERVIEW_GUIDE.md) answers implementation-specific design questions, and
the [final audit](docs/AUDIT.md) records corrections and verification gates.

## Build and use

Linux is required (`epoll`, `eventfd`, and `signalfd`).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

./build/cache_server --socket /tmp/local_cache.sock --workers 8 \
  --max-entries 100000 --max-cache-bytes 256m --max-value-bytes 1m
./build/cache_client put user:123 'binary-safe-value'
./build/cache_client get user:123
./build/cache_client stats
```

Run `cache_server --help` for all connection, queue, cache, and shutdown limits.

## Tests and sanitizers

```bash
ctest --test-dir build --output-on-failure
scripts/run_sanitizers.sh
```

Tests cover LRU promotion/eviction/accounting, defensive and fragmented parsing, binary round trips,
real-socket operations/restart, concurrent clients, and 100,000 sustained insertions under a small
cache limit. The sanitizer script uses separate ASan+UBSan and TSan builds because ASan and TSan
cannot be combined.

## Benchmark and 10k+ stress validation

`cache_benchmark` measures the complete client/serialization/socket/epoll/worker/cache return path.
It pre-populates the key space, uses a fixed random seed, and reports average, p50, p95, p99, max,
throughput, failures, and observed GET hit rate.

```bash
scripts/run_benchmark.sh
scripts/run_stress.sh                 # 500 sockets x 32 pipeline = 16,000 in flight
```

Environment variables documented in the scripts adjust clients, workload, and request count. Use a
Release build on an otherwise idle Linux host. Record CPU, kernel, compiler, worker/client counts,
workload mix, request count, and the unedited output when using results in a resume. This repository
does not contain invented benchmark numbers; performance claims are supported only by output from
the machine on which these commands are run.

The latest measured run is recorded in
[docs/BENCHMARK_RESULTS.md](docs/BENCHMARK_RESULTS.md), including its environment and reproduction
commands. Treat it as evidence for that environment, not a guarantee for every host.

The stress test first pipelines deterministic binary PUTs and then pipelines GETs for all keys. It
byte-compares each response and reports protocol, connection, validation, and data-corruption
failures. Configure the server's `max-entries` to at least the stress request count, as the script
does, so intended eviction is not mistaken for corruption.

## Memory accounting

`current_cache_bytes` includes key bytes, value bytes, and a conservative fixed 128-byte estimate
per entry for list/map nodes, string objects, bucket share, and allocator metadata. It excludes
allocator fragmentation, excess bucket capacity, and transport/worker memory. This is an enforceable
cache budget, not an exact process-RSS measurement. The connection count, frame sizes, in-flight work,
queue, and output backlog are separately bounded.

## Engineering trade-offs and future work

The single mutex gives simple, exact global LRU semantics; sharding would raise throughput but create
per-shard eviction order and capacity imbalance. The custom binary protocol is compact, deterministic,
and binary-safe, while requiring bespoke tooling and version discipline compared with JSON. `epoll`
avoids a thread stack and scheduler cost per client, at the cost of explicit state machines.

Useful next steps are per-client credentials/authorization (`SO_PEERCRED`), histogram-based production
telemetry, optional TTLs, `io_uring` evaluation, cache sharding behind an explicit semantic contract,
and fuzzing the incremental parser.
