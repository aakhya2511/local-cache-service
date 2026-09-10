# Final external audit

Audit date: 2026-09-09. Verdict: **PASS WITH CORRECTIONS**; all required corrections listed below
were applied and revalidated before the freeze commit.

## Findings and corrections

1. Shutdown originally closed the listener but continued accepting work from existing sockets and
   considered a connection drained without counting executing jobs. Reads are now disabled during
   drain and the predicate includes per-connection in-flight work.
2. Malformed-request replies recovered the request ID but always used PING as the echoed opcode. Both
   clients now validate opcode correlation, and the server recovers and echoes a valid header opcode.
3. The asynchronous benchmark registered `EPOLLOUT` permanently. It now enables writable interest
   only for unsent bytes, avoiding measurement-distorting writable wakeups.
4. Ordering, ownership, benchmark timing, and resource limits were accurate in code comments but
   incomplete in project documentation. They are now explicit in `DESIGN.md`.
5. The proposed phrase “binary serialization for cache entries” was not literal: in-memory entries
   are `std::string` byte containers; request/response representations are serialized. Corrected
   resume wording appears below.

## Architecture and concurrency conclusion

The implemented path is AF_UNIX/SOCK_STREAM → non-blocking accepted sockets → level-triggered epoll →
incremental per-connection framing → bounded worker deque → mutex-protected exact LRU → staged
response → eventfd → event-loop send. There is no thread-per-client fallback. Workers never perform
socket I/O.

Connection lifetime is safe: the map owns the ordinary reference, jobs own `shared_ptr`s, the event
loop marks a connection closed before erasing it, and late workers discard output. Because the old
object continues owning its descriptor until the final reference, the descriptor cannot be reused
while a late worker still refers to that connection. Cache list nodes are stable under splice, and
the map's `string_view` is erased before its owning list node.

The completion notification protocol has no lost-wakeup window: a worker stages bytes before claiming
the atomic notification bit; the event loop clears that bit before flushing. A concurrent later
completion is either included in that flush or schedules another eventfd wakeup.

No lock-order cycle was found. Cache, output staging, completion-ready, and worker-queue locks are not
held across one another in request handling. Worker shutdown stops acceptance under the queue mutex,
wakes sleepers, and joins active workers outside that mutex.

## LRU conclusion

GET/DELETE have average O(1) hash lookup and constant-time list operations. PUT has average O(1)
lookup/insertion and O(1) per eviction; a single byte-capacity insertion can evict multiple entries,
making it O(k) for that call but amortized O(1) across insertions because each entry can be evicted
once. GET must take the mutex because a hit splices its node to MRU and therefore mutates shared list
links and ordering.

Overwrite removes the old map view and node, subtracts its exact accounted cost, makes room using the
new cost, and inserts at MRU. Larger and smaller replacements therefore use the correct new size.
Delete, eviction, and clear keep payload/accounted counters consistent; eviction is cumulative across
clear by design. Limits reject oversized/unfit entries and enforce both bytes and count.

## Protocol and sockets conclusion

The documented header exactly matches encoding: 4-byte magic, four 1-byte fields, 8-byte request ID,
4-byte key length, and 4-byte value length (24 bytes total), all multibyte fields big-endian. Fields
are appended/read individually; no ABI struct representation is used. Header and configured/hard
length validation precedes string allocation. With 64-bit `size_t`, the validated uint32 lengths
cannot overflow frame arithmetic.

Partial headers/payloads return NEED_MORE_DATA. The connection retains fragments, extracts coalesced
frames repeatedly, and performs only one LT read per dispatch for fairness. Sends loop over partial
writes until drained or `EAGAIN`; `MSG_NOSIGNAL` prevents SIGPIPE termination. EOF/fatal errors remove
the descriptor from epoll and mark the connection closed. Stale paths are unlinked only when they are
sockets without a live listener; non-socket paths are refused.

## Validation evidence

Commands executed in Ubuntu 24.04 ARM64 with GCC 13.3:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure

cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --parallel
ctest --test-dir build-debug --output-on-failure

cmake -S . -B build-asan-ubsan -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
cmake --build build-asan-ubsan --parallel
ctest --test-dir build-asan-ubsan --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON
cmake --build build-tsan --parallel
setarch aarch64 -R build-tsan/cache_tests --gtest_filter='ConcurrencyTest.*'
```

- Release: 16/16 tests passed in 0.52 seconds.
- Debug: 16/16 tests passed in 0.73 seconds.
- ASan+UBSan: 16/16 passed in 102.69 seconds, no findings.
- TSan: 2/2 concurrency tests passed in 1.373 seconds, no race report. Disabling ASLR was required
  only for TSan shadow-memory placement in the ARM64 Docker VM.
- 32 independent CLI processes completed concurrent PUT and GET waves successfully.

The audited benchmark rerun completed 100,000 requests with zero failures: 0.980 ms average, 0.517 ms
p50, 3.091 ms p95, 7.265 ms p99, 72.196 ms maximum, and 101,905 requests/second. The 100-client,
95%-GET/5%-PUT workload had a 100% GET hit rate. This supports an environment-specific under-1-ms
average result but is close enough to the threshold that it should never be presented as a universal
latency guarantee.

The stress rerun used 500 connections × 32 sent-but-unmatched requests, targeting 16,000 outstanding
at once. Two phases sent 100,000 deterministic binary PUTs and 100,000 GETs. All 200,000 responses
arrived with zero validation, protocol, connection, I/O, or byte-comparison corruption failures.

## Resume conclusions

The first proposed bullet is supported, although including the measured environment and client count
makes it more defensible. The second requires correction: the cache payload is binary-safe but only
its IPC representation is serialized, and “concurrent requests” can be misread as CPU-simultaneous
execution rather than outstanding pipelined work.

Recommended wording:

> Served cached lookups at 0.98 ms average end-to-end latency in a 100-client Release benchmark by
> building an exact-LRU cache behind epoll-driven Unix domain socket IPC.

> Bounded cache memory with entry/byte caps and LRU eviction; validated binary-safe IPC with 16,000
> simultaneously outstanding pipelined requests and 200,000 responses with zero detected corruption.

## Remaining limitations

- Linux-only by design.
- Exact global LRU serializes all hits on one mutex.
- Per-connection pipelined execution is not ordered; request IDs provide correlation, not sequencing.
- Accounted cache bytes are a conservative budget, not total RSS or exact allocator accounting.
- Worker response allocation failure is process-fatal rather than converted to INTERNAL_ERROR.
- Authentication, TTL, persistence, replication, and production telemetry are intentionally out of
  scope.
