# AGENTS.md

## What this is

A URL shortener written in **C**, built as a **learning project**. The goal is to
practise systems programming and to *measure* why the original requirements
(100M URLs/day, 1B reads/day, 10-year retention) do not fit on the target
machine: **1 vCPU, 4 GB RAM, 50 GB NVMe, 4 TB bandwidth**.

Do not describe this as production-ready. Two of the requirements are missed by
design of the hardware, and `docs/LOAD_TEST_RESULTS.md` says which.

## State

C sources under `src/`, Criterion tests under `tests/`, `Makefile`,
`scripts/sandbox.sh`, `config.yaml`. There is no Python in this repo and no
`venv/` — that build was deleted.

## Build and test

Everything runs **inside a capped Docker sandbox**. The host has 16 cores and
19 GB; running tests directly would measure the wrong machine.

```bash
./scripts/sandbox.sh verify   # prove the 1 vCPU / 4 GB / 50 GB caps are imposed
./scripts/sandbox.sh test     # unit + integration tests (2 GB disk profile)
./scripts/sandbox.sh test-disk# retention demo (full 50 GB profile)
./scripts/sandbox.sh e2e      # server + e2e over the network
./scripts/sandbox.sh bench    # load test inside the capped sandbox
./scripts/sandbox.sh teardown # remove containers and network
```

Inside the sandbox container you can also use `make build`, `make test`,
`make test-asan`, `make bench`.

## Gotchas

- **`nproc` is not proof of a CPU cap.** `--cpus=N` sets a CFS bandwidth quota,
  readable at `/sys/fs/cgroup/cpu.max`. `nproc` reads CPU *affinity* and will
  report the host's core count from inside the container. `sandbox.sh verify`
  reads `cpu.max` and confirms throttling via `cpu.stat`'s `nr_throttled`.
- **`/proc/meminfo` is not proof of a memory cap** either; use
  `/sys/fs/cgroup/memory.max`.
- **CPU shares must sum to 1.0** across the postgres/redis/app containers
  (0.5 / 0.15 / 0.35), so contention matches the 1-vCPU target.
- **Postgres data lives on a size-capped `--tmpfs`**, not a named volume, so
  disk-full behaviour is observable and nothing needs cleaning up.
- **Address containers by name** (`sb_pg`, `sb_redis`) on the `shortener_net`
  network. `localhost` inside a container is that container.
- **`-lpq`, `-lhiredis`, `-lcriterion` come from Debian trixie packages**
  (`libpq-dev`, `libhiredis-dev`, `libcriterion-dev`), not from a manifest.
  The sandbox installs them; installing on the host would diverge from what CI runs.
- **`ulimit` is not a cgroup limit.** They behave differently and `ulimit -v`
  does not stop a runaway Postgres.
