# ENGINEERING FAQ

1. **Why Unix domain sockets instead of TCP?** The service is machine-local. UDS avoids routing
   configuration, usually costs less than loopback TCP, and exposes filesystem permissions.
2. **Why not shared memory?** Shared memory could reduce copies but needs a cross-process allocator,
   synchronization, crash recovery, and a safe variable-length layout. Stream IPC has a smaller
   correctness surface.
3. **Why epoll?** One thread multiplexes thousands of readiness-driven sockets without one stack and
   scheduler entity per connection.
4. **Why level-triggered?** LT permits one bounded read per dispatch for fairness and re-reports unread
   data. ET can reduce wakeups but requires reliably draining every descriptor to `EAGAIN`.
5. **Why workers if epoll provides concurrency?** epoll reports readiness; it does not execute cache
   work in parallel. Workers keep handling off the sole event-loop thread.
6. **Why not a thread per connection?** Thousands of stacks and runnable threads waste memory and
   increase scheduling overhead while most sockets are idle.
7. **How is LRU O(1)?** A hash map finds a list iterator in average O(1); list splice, insertion, and
   tail eviction are constant-time.
8. **Why does GET lock?** A hit moves its node to MRU, mutating list links and exact global order.
9. **What happens when the queue fills?** The request receives `SERVER_BUSY`; the deque stays bounded.
10. **How are allocation attacks bounded?** Header lengths are validated before payload allocation,
    with protocol ceilings and connection, in-flight, queue, output, and cache limits.
11. **Partial reads?** Bytes accumulate in a connection vector; parsing returns NEED_MORE_DATA until a
    validated frame is complete.
12. **Partial writes?** The loop advances an offset until completion or `EAGAIN`, retains the remainder,
    and enables `EPOLLOUT`.
13. **Why binary rather than JSON?** It is compact, deterministically framed, and binary-safe; the cost
    is custom tooling and version discipline.
14. **Why define endianness?** Big-endian encoding is independent of host architecture and compiler ABI.
15. **How are pipelined responses mapped?** A 64-bit request ID is echoed; clients match pending work by
    ID and validate the echoed opcode.
16. **Are operations on one connection ordered?** Parsing and FIFO dequeue are ordered; execution and
    completion are not. Dependent operations must await their predecessors.
17. **Biggest scalability bottleneck?** The mutex needed for exact global LRU recency.
18. **How would you scale past it?** Shard key space and capacity, accepting per-shard eviction order.
19. **Why not lock-free?** Exact LRU mutates linked global order on every hit; lock-free reclamation and
    multi-object invariants add disproportionate risk.
20. **How do you know there are no races?** Ownership minimizes sharing; shared state uses mutexes or
    atomics, lifetimes use `shared_ptr`, and manual review is backed by focused TSan tests.
21. **How was under 1 ms measured?** In Release through serialization, UDS, epoll, worker scheduling,
    LRU, and the complete response path—not a direct cache call.
22. **Why is average insufficient?** It hides queueing and scheduling tails, so p50/p95/p99/max are also
    reported.
23. **Why is p99 higher?** Worker scheduling, lock/queue contention, event batching, VM scheduling, and
    socket buffering create tails even when cache access is fast.
24. **What does 16,000 in-flight mean?** 500 sockets each held 32 sent but unmatched requests. It does
    not mean 16,000 threads or CPU operations executing simultaneously.
25. **How was corruption detected?** The stress tool writes deterministic 256-byte binary values,
    retrieves every key in a second pipelined phase, and byte-compares every response.
26. **What does the cache memory limit count?** Key/value payload plus a conservative 128-byte per-entry
    estimate for container and allocator overhead.
27. **Does that equal RSS?** No. It excludes fragmentation, excess buckets, transport buffers, stacks,
    code, and libraries; separate controls bound the major non-cache accumulators.
28. **What happens on SIGTERM?** `signalfd` wakes epoll; listening and reads stop, accepted work drains to
    a deadline, workers join, connections close, and the socket path is unlinked.
29. **Disconnect during execution?** The worker's `shared_ptr` prevents use-after-free. A closed flag
    discards its finished response, and RAII releases the descriptor with the final reference.
30. **What changes for production?** Tune limits together, authorize peers with credentials, add
    operational histograms, fuzz the parser, and shard only after profiling real contention.
