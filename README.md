# URL shortener, in C

A URL shortener built to practise systems programming, and to **measure** why the
original requirements do not fit the machine they were asked to run on.

**This is a learning project, not a production service.** It is deliberately not
described as production-ready. The mechanism is correct; the hardware is 20× too
small for the retention requirement and 2× too small for the read bandwidth. The
point of building it is the measured gap, not a deploy.

---

## The arithmetic, first

This is why the requirements are missed, and it is stated before the
instructions so nothing below is misread as a capacity claim:

```
REQUIREMENT                          THIS MACHINE
100M URLs/day  x 100 B payload   =   10 GB/day raw
                    x3 overhead  =   30 GB/day on disk
10 years of that                 =   36.5 TB
available                        =   50 GB
                                  ->  ~1.3 days of retention

1B redirects/day x 270 B         =   ~8 TB/month egress
available                        =   4 TB/month
                                  ->  ~5,700 reads/sec, not 11,574
```

Two limits bind, and neither is the CPU:

| Requirement | Status on 1 vCPU / 4 GB / 50 GB |
|---|---|
| NFR-1 · 100M URLs/day | **Not met** — a ~5,700 reads/s bandwidth ceiling |
| NFR-3 · 10-year storage | **Not met** — 20× short on disk; the schema is correct |
| NFR-4 · 99.99% HA | **Not met** — one node, no failover |
| NFR-5 · 10-year retention | **Not met** — ~1.3 days on 50 GB; the logic is correct |

The single vCPU is a third constraint, and the one that shapes the code: every
storage call blocks the one core, which is why the server keeps its own I/O in
one epoll loop and its database work in a small pool rather than a thread per
request.

---

## Build and test

Everything runs inside a capped Docker sandbox that **imposes** the target
hardware (1 vCPU, 4 GB, 50 GB). The host is bigger, and running tests directly
would measure the wrong machine — so every command below goes through the
sandbox:

```bash
./scripts/sandbox.sh verify   # prove the caps are imposed (cpu.max, not nproc)
./scripts/sandbox.sh test     # unit + integration tests (2 GB disk profile)
./scripts/sandbox.sh test-asan# the same suite under ASan + UBSan
./scripts/sandbox.sh test-disk# full 50 GB profile
./scripts/sandbox.sh e2e      # the server answering a real client over a socket
./scripts/sandbox.sh bench 30 # the four load scenarios; writes docs/LOAD_TEST_RESULTS.md's inputs
./scripts/sandbox.sh test-diskfull  # fill the disk until Postgres refuses (retention)
./scripts/sandbox.sh teardown # remove the containers and the network
```

Inside the sandbox container you can also use `make build`, `make test`,
`make test-asan` and `make test-e2e` directly.

`nproc` is **not** proof of the CPU cap: `--cpus=N` sets a CFS bandwidth quota
that `nproc` cannot see. `sandbox.sh verify` reads `/sys/fs/cgroup/cpu.max`
instead, and confirms the quota actually throttles.

---

## Run it

```bash
docker compose up -d --build
curl -s localhost:8000/health
```

The compose file runs the same three processes as the sandbox with the same
share of the machine — Postgres 0.5 CPU, Redis 0.15, the app 0.35, summing to
the single core; 1 GB, 256 MB and 1 GB of the 4 GB. Under load they compete,
which is what makes the throughput a single-core number.

The server reads `config.yaml` for defaults and the environment for overrides
(`DATABASE_URL`, `REDIS_URL`, `BASE_URL`, `PORT`). The compose file sets the DSNs
to service names; `config.yaml` names the sandbox's containers (`sb_pg`,
`sb_redis`), which only resolve on the sandbox network.

---

## The API

```bash
# Create a short URL. ttl_days is optional; the default is config.yaml's.
curl -s -X POST localhost:8000/api/v1/shorten \
     -H 'Content-Type: application/json' \
     -d '{"url":"https://example.com/some/long/path"}'
# {"code":"0000007","short_url":"https://short.ly/0000007"}

# A custom code (4–12 alphanumeric characters) is optional.
curl -s -X POST localhost:8000/api/v1/shorten \
     -H 'Content-Type: application/json' \
     -d '{"url":"https://example.com","custom_code":"launch","ttl_days":30}'

# Follow a code. 301 with the target in Location; 404 when unknown or expired.
curl -s -o /dev/null -D - localhost:8000/0000007

# Liveness and counters.
curl -s localhost:8000/health     # -> ok
curl -s localhost:8000/metrics    # Prometheus text format
```

Rate limiting applies to writes only — `POST /api/v1/shorten` — not to reads or
redirects, because the read path is the one the bandwidth ceiling already bounds.
A request past the budget gets `429` with `Retry-After`.

### Retention

Expired rows are removed by a job, not by a request:

```bash
docker compose exec app /app/shortener --cleanup
# cleanup: 3 expired row(s) removed
```

The job deletes expired rows in batches and drops each deleted code from the
cache in the same sweep, because the cache is a second copy of the data and a
cached entry would otherwise keep answering for a row that no longer exists. It
is idempotent; run it from cron.

---

## Layout

```
src/
  net/            epoll reactor and connection buffering
  http/           HTTP/1.1 parser and response writer
  api/            router, handlers, and the assembled application
  services/       URL create/lookup and the token-bucket rate limiter
  db/             libpq connection pool and schema
  cache/          hiredis cache
  jobs/           the retention sweep
  observability/  Prometheus counters and JSON logging
tests/            Criterion unit and integration tests
bench/            the load generator (Task 10)
docs/             measured results, including LOAD_TEST_RESULTS.md
```

The measured throughput, latency and retention numbers live in
`docs/LOAD_TEST_RESULTS.md`, with the constraints stated alongside them.

See `AGENTS.md` for the sandbox gotchas (`nproc`, `/proc/meminfo`, why CPU
shares must sum to 1.0) before changing anything under `scripts/`.
