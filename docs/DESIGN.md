# Design

## Boundaries and ownership

`Server` owns the listener, epoll/event/signal descriptors, connection map, worker pool, statistics,
and cache. `Connection` owns one accepted descriptor and its stream buffers. The event-loop thread is
the only socket reader/writer. Workers hold `shared_ptr<Connection>` only while a request is active,
stage serialized responses under a small output mutex, and notify the loop with `eventfd`. RAII closes
every descriptor.

| Object | Owner | Accessing threads | Synchronization / lifetime boundary |
|---|---|---|---|
| `Server` event-loop state and connection map | daemon main thread | event-loop only | no shared mutation; lives through `run()` |
| connection socket and input/write buffers | `Connection` | event-loop only | descriptor lives until the final `shared_ptr` is released |
| staged response bytes | `Connection` | workers append; event loop swaps | `out_mutex_` |
| connection flags and counters | `Connection` | event loop and workers | atomics; event-loop-only flags need no lock |
| completion-ready vector | `Server` | workers and event loop | `ready_mutex_` plus one pending notification per connection |
| job deque | worker pool | event loop and workers | pool mutex and condition variables |
| LRU list, index, accounting | cache | all workers | one cache mutex; no iterator or reference escapes |
| statistics | `ServerStats` | event loop and workers | relaxed atomics; never used for synchronization |

The server intentionally uses level-triggered epoll. One read is performed per readiness dispatch to
prevent a hot client monopolizing the loop; unread bytes remain readable and are reported again. On
writes, the connection drains until `EAGAIN`, then enables `EPOLLOUT`. Stream parsing retains partial
frames and consumes any number of complete coalesced frames.

Workers never call `send` or mutate event-loop buffers. They append a complete response to the
staging buffer, atomically claim a notification slot, add the connection to the ready vector, and
increment the completion `eventfd`. The loop clears the notification bit before flushing. A response
staged concurrently after that clear either appears in the current flush or claims a fresh wakeup;
there is no lost-notification window.

## Protocol version 1

All integers are unsigned and big endian. No C++ structure is copied to or from wire memory.

| Offset | Bytes | Request | Response |
|---:|---:|---|---|
| 0 | 4 | magic `0x4c435331` | same |
| 4 | 1 | version (`1`) | version (`1`) |
| 5 | 1 | opcode | echoed opcode |
| 6 | 1 | flags (`0`) | flags (`0`) |
| 7 | 1 | status (`0`) | status code |
| 8 | 8 | request ID | correlated request ID |
| 16 | 4 | key length | zero |
| 20 | 4 | value length | response payload length |
| 24 | variable | key, then value | value |

Opcodes are GET=1, PUT=2, DELETE=3, PING=4, STATS=5, CLEAR=6. Status values are OK,
NOT_FOUND, INVALID_REQUEST, VALUE_TOO_LARGE, SERVER_BUSY, INTERNAL_ERROR, and
UNSUPPORTED_VERSION. GET/PUT/DELETE require a nonempty key; only PUT accepts a request value.
The absolute limits are 64 KiB keys and 64 MiB values, with normally smaller configured limits.
Lengths are validated before payload allocation. A bad header makes stream resynchronization
ambiguous, so the server optionally replies using a recoverable request ID and closes after flushing.

STATS is a version-tolerant field-count followed by big-endian 64-bit counters. Readers consume known
prefix fields and tolerate appended fields.

## Cache invariants and locking

The list owns keys and values and is ordered MRU-to-LRU. The hash map key is a `string_view` into a
stable list node and maps to its iterator. Erasing always removes the map view before destroying its
list node. `splice` preserves node/string addresses. All operations use one mutex because GET changes
recency; returned values are copies. Thus map/list membership, byte counters, and ordering change as
one transaction without exposing invalidatable state.

An entry costs `key.size + value.size + 128` accounted bytes. PUT rejects a value beyond its value
limit or an entry that cannot fit in an empty cache, then evicts LRU nodes until count and byte limits
both hold. This conservative portable estimate is documented rather than presented as exact RSS.

## Backpressure and bounded resources

- accepted connections are capped;
- protocol fields and complete frames have hard maxima;
- unanswered requests per connection are capped;
- the shared worker queue has a fixed maximum and returns SERVER_BUSY when full;
- reads pause above the per-connection output watermark;
- a hard output ceiling closes clients that do not drain responses;
- cache entries, values, and accounted bytes are independently capped.

These are separate budgets: bounding only the cache would not protect the daemon from slow clients or
request floods.

| Resource | Limit | Enforcement | Overload behavior |
|---|---:|---|---|
| kernel accept backlog | `accept_backlog` | `listen()` | kernel queues or refuses connections |
| accepted connections | `max_connections` | accept loop | accept then close; rejection counted |
| input per connection | maximum frame plus one 64 KiB read chunk | lengths validated before allocation | malformed/oversized stream is rejected and closed |
| unanswered requests per connection | `max_inflight_per_conn` | remove `EPOLLIN` interest | reads resume after completions |
| queued jobs | `max_pending_requests` | bounded deque | immediate `SERVER_BUSY` |
| output per connection | soft configured watermark and post-append hard threshold | pause reads, then close | slow client cannot grow output indefinitely |
| completion-ready vector | one notification per connection | atomic coalescing bit | bounded by connection count |
| worker response buffers | one retained vector per worker | fixed workers and frame maximum | allocation failure remains process-fatal |
| cache | entry, accounted-byte, and value limits | repeated LRU eviction | oversized entry rejected |
| benchmark outstanding work | connections × pipeline depth | per-connection pending map | generation waits for responses |

The limits are finite rather than necessarily small. Queue depth multiplied by maximum request size
is the queue's worst-case payload footprint, so deployment configuration must choose them together.

## Request and response ordering

The event loop parses stream bytes and submits complete requests in arrival order. Workers pop the
global queue FIFO, but several workers execute concurrently; cache-operation and response-completion
order are not preserved for one connection. Responses carry the original request ID and are staged in
completion order, so a pipelining client must correlate rather than assume FIFO responses.

For rapidly pipelined `PUT(key,A)`, `PUT(key,B)`, `GET(key)`, worker scheduling permits the GET to
observe a missing value, A, or B, and the final value may be A or B. A client needing sequential
semantics must await each predecessor; the synchronous `CacheClient` naturally does this.

## Shutdown and failures

SIGINT/SIGTERM are blocked before worker creation and consumed through `signalfd`. Shutdown removes
the listener and socket path, disables reads on existing connections, gives already queued/in-flight
work a configured drain interval, joins workers, marks connections closed, and releases descriptors.
The drain-complete predicate includes queued jobs, executing requests, and pending response bytes.
`MSG_NOSIGNAL` turns peer closure
into ordinary I/O errors. Stale startup sockets are removed only if they are sockets and no live
server accepts a probe connection; non-socket paths are never unlinked.

## Alternatives

Unix sockets were chosen over TCP loopback for local-only semantics, no routing stack requirement,
lower IPC overhead, and filesystem permissions. TCP would make remote deployment and cross-platform
support easier.

A mutex-protected global LRU favors correctness and exact eviction order. Shards could reduce cache
lock contention but each shard would have its own approximate capacity/recency behavior.

The custom protocol avoids text conversion, supports NUL-containing values, has small deterministic
frames, and permits early length rejection. JSON would improve human inspection and ecosystem
interoperability but add encoding rules for binary values and more parsing/bytes.

An epoll loop avoids one thread and stack per client, enabling thousands of descriptors with a small
thread count. It requires careful per-connection buffering and readiness bookkeeping that a blocking
thread-per-connection model would hide.

`epoll` supplies scalable I/O readiness, not parallel cache execution. The worker pool prevents cache
handling from stalling every socket while keeping thread and queue counts fixed. The global exact-LRU
mutex is the primary throughput bottleneck; sharding is the direct scale-out option if per-shard
rather than exact global eviction semantics are acceptable.

## Benchmark methodology

The benchmark pre-populates through the public synchronous client, then uses non-blocking Unix sockets
and epoll to maintain configurable outstanding requests. A timer begins when a request is serialized
into a connection output queue and ends when the matching response is fully received and parsed.
Pipelined latency therefore includes client send-queue waiting, both IPC legs, server framing and
queueing, worker scheduling, cache execution, and response parsing. `EPOLLOUT` is registered only
while a partial request remains unsent.

Samples use `steady_clock`, are sorted, and percentiles use nearest rank: `ceil(p * N) - 1`. Average
is the arithmetic mean. Average supports the resume threshold; p50, p95, p99, and maximum expose
scheduling and queueing tails it can hide.
