# Load test results

Throughput and latency come from `./scripts/sandbox.sh bench`, on the target
machine's caps: **1 vCPU shared three ways** (Postgres 0.5, Redis 0.15, the app
0.35) and **4 GB RAM**. The bytes-per-row figure comes from
`./scripts/sandbox.sh test-diskfull`, which fills a size-capped tmpfs until
Postgres refuses to write. Reproduce both with:

```bash
./scripts/sandbox.sh bench 30
./scripts/sandbox.sh test-diskfull
```

Two limits shape how to read these figures, and neither is the server's code:

- **The generator shares the app's 0.35 vCPU.** It runs inside the app container
  (`docker exec sb_app ./bin/load_test`), not on the host. The host has 16 cores;
  a generator there would have measured a machine that does not exist. So the
  throughput below is what *both together* sustained, which makes it a **floor**,
  not a ceiling.
- **The server is synchronous.** One epoll thread issues a blocking libpq and
  hiredis call per request, so a request waits for its own storage round trip.
  This is a deliberate simplification, documented in `src/services/url_service.h`,
  not an optimisation left undone.

## The measurement that matters most: what a Postgres round trip costs

Same 2,000 codes, same shape of traffic — 2,000 reads, each code read exactly
once — run twice. The only difference is whether the cache had the answer.

| | p50 | p95 | p99 | throughput |
|---|---|---|---|---|
| **Cold** (every read a cache miss) | 444 µs | 560 µs | 853 µs | 1,812 req/s |
| **Warm** (cache primed first) | **63 µs** | 89 µs | 115 µs | **6,597 req/s** |

**One Postgres round trip costs about 380 µs on this hardware — 7× a cache hit.**

That ratio is the whole storage story. The cache is not a nice-to-have; it is the
difference between serving 1,812 and 6,597 reads/s on one core.

Repeat runs move these by roughly 5–8%: three containers are time-slicing one
core, so the scheduler decides which of them absorbs the jitter. Treat any single
figure here as ±10%, not as a benchmark to two decimal places.

The cold run is only possible because the codes are inserted straight into
Postgres by the harness (`seed_codes` in `scripts/sandbox.sh`) rather than created
through the API. Create is write-through, so an API-created code is *already
cached* and its first read is a hit — a "cold" run built that way measures a warm
one. The generator has no database access and cannot flush Redis, so this is the
only arrangement that hands it genuinely cold codes.

### The counters prove which path served each read

Latency alone cannot tell a cold run from a warm one, so the server counts cache
hits and misses, and the harness scrapes `/metrics` around each scenario:

| scenario | hits | misses |
|---|---|---|
| cold reads | +0 | **+2,000** |
| warm reads | **+2,000** | +2,000 (the priming pass, which is not timed) |
| writes only | +0 | +0 |
| mixed 10:1 | **+150,389** | +0 |

The cold run's 2,000 misses and the warm run's 2,000 hits are the same 2,000
requests. That is the proof the two rows above are measuring different things.

The mixed run's zero misses is the other half of the story: 150,389 reads, no
misses, because every code it read was one it had just written and the create
path is write-through. **Steady-state read traffic is warm traffic**, and the
cold path is what a cache flush, a cold start or a 24 h TTL expiry looks like.

## Scenarios

```
scenario: threads, reads + writes per cycle, duration, pool size, primed passes
elapsed:  wall-clock seconds

           ok        err        req/s   p50      p95      p99
reads  2000          0        1812.1    444us    560us    853us
writes     0          0           0.0      -        -        -
all     2000          0        1812.1    444us    560us    853us
statuses: 301=2000
```

### Writes only — 3,282/s

```
  writes   98468 ok        0 err    3282.3 req/s   p50 266us  p95 343us  p99 486us
  statuses: 201=98468
```

98,468 URLs in 30 s, **0 errors, 0 reconnects**. Each write is a sequence for the
id, an `INSERT`, and a cache write — 266 µs at the median, versus 63 µs to read
the same code back warm. **A write costs about 4× a cache hit**, and the
requirement asks for 1,157 writes/s sustained; this does 3,282/s with the
generator taking a third of the same core.

### Mixed 10:1 — 5,509 req/s total

Two threads, 10 reads per write:

```
  reads  150389 ok        0 err    5008.3 req/s   p50 111us  p95 334us  p99 481us
  writes  15039 ok        0 err     500.8 req/s   p50 337us  p95 613us  p99 978us
  all    165428 ok        0 err    5509.2 req/s   p50 114us  p95 377us  p99 624us
  statuses: 201=15039 301=150389
```

165,428 requests, **0 errors, 0 reconnects**. Note the read p99 (481 µs) is under
twice its p50: with a working cache the tail is bounded by the occasional write
waiting on Postgres, not by queueing.

## Retention: how much history 50 GB actually holds

From `./scripts/sandbox.sh test-diskfull`, filling a size-capped tmpfs until
Postgres refuses to write:

```
  rows written before the failure : 14867
  elapsed                          : 4 s
  rate                             : 3717 inserts/s
  declared tmpfs                   : 0.12 GiB
  bytes per row (incl. WAL+index)  : 9028
  rows per GiB                     : 118936
```

**9,028 bytes per row**, at a maximum-length URL, including WAL and indexes.
That is 30× the 300 B/row the original arithmetic assumed (100 B payload × 3
overhead). The column cap, the two indexes, the composite primary key, the
partitioning overhead and the WAL are all real costs the back-of-envelope number
did not have.

So 50 GB holds:

```
  50 GiB x 118,936 rows/GiB  =  5,946,800 rows
```

At the required 100M URLs/day:

```
  5,946,800 / 100,000,000   =  0.059 days  =  ~1.4 hours
```

**Not 1.3 days — about 86 minutes.** The plan's estimate was 31.2 hours of
history; the measured 1.4 makes it **~22× optimistic**, because 300 B/row ignored
indexes, WAL and row overhead.

## Where each requirement lands

| Requirement | Asks for | Measured | Met? |
|---|---|---|---|
| NFR-1 · reads | 5,700/s (the figure the requirement's own arithmetic reaches) | 6,597/s warm, 1,812/s cold | Warm yes, cold no |
| NFR-1 · writes | 1,157/s | 3,282/s | Yes, 2.8× |
| NFR-2 · mixed | 90th pct < 100 ms | p99 624 µs | Yes, ~160× headroom |
| NFR-3 · storage | 36.5 TB (10 yr) | 50 GB ≈ 5.9M rows | **No, 730× short** |
| NFR-4 · availability | 99.99% | single node, no failover | **No** |
| NFR-5 · retention | 10 years | ~1.4 hours | **No, ~61,000× short** |

**The disk and the bandwidth ceiling bind; the CPU does not.** Writes beat their
requirement by 2.8×, and the mixed-workload p99 sits ~160× inside its budget.
Storage is the requirement that fails hardest, and it fails by two orders of
magnitude.

### The read number, stated honestly

The requirement says 11,574 reads/s, but its own egress arithmetic only supports
5,700/s on 4 TB/month. Both numbers are in the plan; the second is the one derived
from the bandwidth the machine actually has.

Against 5,700/s, the warm measurement (6,597/s) clears it by 16% — inside the
±10% run-to-run noise. But the generator was sharing the same 0.35 vCPU, so
6,597/s is what the *pair* sustained; the server's own ceiling is higher and was
not isolated. **The measurement cannot tell you how much higher.** What it does
establish is the cache: the identical run against uncached codes does 1,812/s, so
**a low cache hit rate is the single thing that breaks the read requirement**, and
the original design assumed a hot cache without saying so. That assumption, not
the CPU, is the load-bearing error.

## What was deliberately not measured

- **No multi-worker run.** `app.workers` is 1. On one core more threads buy
  context switching; this is stated rather than tested to avoid presenting a
  scheduling artifact as capacity.
- **No TLS.** A 4 TB/month egress figure with handshakes would measure the CPU,
  which is not the binding constraint.
- **The rate limiter is open for the benchmark.** At the shipped 60 writes/minute
  the benchmark would measure 429s — the policy's answer rate, not the machine's
  capacity. `bench/bench-config.yaml` differs from `config.yaml` only in that
  field, and the limiter's own behaviour is tested in `tests/test_rate_limit.c`.
- **The load generator is closed-loop**, so a slow server shows up as latency
  rather than as queue depth. It cannot distinguish "the server is busy" from
  "the server is broken"; it can only report what a client would have experienced.

## Reproducing the storage figure

```bash
./scripts/sandbox.sh test-diskfull
```

The URL bodies in that test are generated by an LCG on purpose. An earlier
version filled them with repeated `a`, which Postgres TOASTed through pglz down to
almost nothing and reported 709,147 rows in 256 MB — 361 B for a 2,048 B URL,
understating the disk cost sixfold. A compressible row would have made the
retention lesson here wrong in the flattering direction.