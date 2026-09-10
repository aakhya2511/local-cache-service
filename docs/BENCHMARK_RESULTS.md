# Measured validation results

Run date: 2026-09-09. These are observed results, not constants embedded in either executable.

## Environment

- Host: Apple ARM64 Mac, Linux workload run through the local Docker VM
- Container: Ubuntu 24.04, ARM64, 6 visible CPU cores
- Kernel visible to container: Linux 6.8.0-117-generic, PREEMPT_DYNAMIC
- Compiler: GCC 13.3.0
- Build: CMake `Release`
- Server: 8 workers, 250,000 max entries, 256 MiB accounted cache capacity
- Client: 100 connections, pipeline depth 1
- Workload: 100,000 requests, 10,000 pre-populated 128-byte values, 95% GET / 5% PUT
- Observed GET hit rate: 100.00%

## End-to-end result

```text
Successful:          100000
Failed:              0
Duration:            0.981 s
Throughput:          101905 req/s

Average:             0.980 ms
P50:                 0.517 ms
P95:                 3.091 ms
P99:                 7.265 ms
Max:                 72.196 ms
```

This measures serialization, Unix-socket IPC, epoll dispatch, worker scheduling, LRU access, response
serialization, and return IPC. It supports the under-1-ms average claim for this controlled run; the
tail percentiles are not under 1 ms.

Reproduce with:

```bash
LCS_WORKERS=8 LCS_CLIENTS=100 LCS_PIPELINE=1 LCS_REQUESTS=100000 \
LCS_KEY_SPACE=10000 LCS_READ_PERCENT=95 LCS_PUT_PERCENT=5 \
  scripts/run_benchmark.sh
```

## 16,000-in-flight corruption validation

The Release server used 8 workers, 100,000 maximum entries, and 256 MiB cache capacity. The stress
client used 500 socket connections and pipeline depth 32. It sent 100,000 deterministic binary PUTs,
then 100,000 GETs and byte-compared every returned value.

```text
Target in-flight:          16000
Requests sent:             200000
Responses received:        200000
Validation failures:       0
Protocol failures:         0
Connection failures:       0
I/O failures:              0
Data corruption failures:  0
```

Reproduce with `LCS_STRESS_REQUESTS=100000 LCS_WORKERS=8 scripts/run_stress.sh`.

## Correctness and sanitizers

- Release Linux build: 16/16 tests passed.
- Debug Linux build: 16/16 tests passed.
- GCC AddressSanitizer + UndefinedBehaviorSanitizer: 16/16 tests passed; no findings.
- GCC ThreadSanitizer: both concurrency tests passed; no race reports. In the Docker VM, TSan needed
  ASLR disabled for its process (`setarch aarch64 -R`) because its shadow-memory reservation otherwise
  collided with a runtime mapping. This is a test-environment constraint, not a suppressed finding.
