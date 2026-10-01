#!/usr/bin/env bash
# Sandbox harness for the URL shortener.
#
# Adapted from the test-sandbox skill, which has no C recipe. The skill's golden
# rules are kept (:ro mount, copy to a writable path, --rm, container names on a
# shared network, explicit teardown). What this adds is the point of the project:
# the host has 16 cores / 19 GB, and the target machine has 1 vCPU / 4 GB / 50 GB.
# Without caps, tests would measure the wrong machine.
#
# Usage:
#   scripts/sandbox.sh verify       prove the caps are actually imposed
#   scripts/sandbox.sh test         unit + integration tests (2 GB disk profile)
#   scripts/sandbox.sh test-disk    retention demo (full 50 GB profile)
#   scripts/sandbox.sh test-diskfull  fill the disk until writes fail (256 MB)
#   scripts/sandbox.sh e2e          build image, run server + e2e over the network
#   scripts/sandbox.sh bench        load test inside the capped sandbox
#   scripts/sandbox.sh teardown     remove anything this script created

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# The target machine, per the plan.
TARGET_CPUS="${SANDBOX_CPUS:-1}"
TARGET_MEM="${SANDBOX_MEM:-4g}"
DISK_FUNCTIONAL="${SANDBOX_DISK_FUNCTIONAL:-2g}"
DISK_FULL="${SANDBOX_DISK_FULL:-50g}"
# The disk-full demonstration fills Postgres until it refuses to write. Filling
# 50 GB to prove that a 200 MB disk would also refuse it is a waste of an hour,
# so it gets a small tmpfs of its own. Postgres's WAL is capped to match, since
# a default 1 GB max_wal_size would not fit and the failure would be WAL's
# rather than the table's. wal_level is left at the image default: the postgres
# image sets max_wal_senders > 0, and asking for wal_level=minimal alongside it
# is a fatal configuration error rather than a smaller WAL.
DISK_FULL_TEST="${SANDBOX_DISK_FULL_TEST:-128m}"
# min_wal_size must be at least twice the 16 MB WAL segment, which Postgres
# enforces at startup and which would otherwise abort the container before it
# ever accepts a connection.
PG_ARGS_DISK_TEST='-c max_wal_size=64MB -c min_wal_size=32MB -c checkpoint_timeout=30s'

NET=shortener_net
PG=postgres:16-alpine
REDIS=redis:7-alpine
BUILDER=debian:trixie-slim

# CPU shares sum to 1.0 so the three containers contend for one core, matching
# the target box. Without this the app would float on the host's 16 cores.
CPU_PG="${SANDBOX_CPU_PG:-0.5}"
CPU_REDIS="${SANDBOX_CPU_REDIS:-0.15}"
CPU_APP="${SANDBOX_CPU_APP:-0.35}"

PG_MEM="${SANDBOX_PG_MEM:-1g}"
REDIS_MEM="${SANDBOX_REDIS_MEM:-256m}"
APP_MEM="${SANDBOX_APP_MEM:-1g}"

INSTALL_DEPS='export DEBIAN_FRONTEND=noninteractive; \
  apt-get update -qq && apt-get install -y -qq --no-install-recommends \
  gcc make libcriterion-dev libpq-dev libhiredis-dev pkg-config ca-certificates >/dev/null 2>&1'

teardown() {
  docker kill sb_pg sb_redis sb_app >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
}

# Extra `postgres -c` settings for the current profile, set by a cmd_* before
# it calls start_services. A global rather than a parameter because
# run_in_builder's variadic arguments are environment variables for the build
# container, and mixing the two meanings there would silently pass Postgres flags
# to `docker run -e`.
PG_ARGS_EXTRA=""

# "2g"/"256m" to bytes, so a test can check the profile it is actually running
# against instead of trusting the string it was handed.
disk_bytes() {
  awk -v d="$1" 'BEGIN{
    n = d + 0
    if (d ~ /[gG]$/) n = n * 1024*1024*1024
    else if (d ~ /[mM]$/) n = n * 1024*1024
    else if (d ~ /[kK]$/) n = n * 1024
    printf "%d", n
  }'
}

start_services() {
  local disk="$1"
  teardown
  docker network create "$NET" >/dev/null 2>&1 || true

  docker run -d --rm --name sb_pg --network "$NET" \
    --cpus="$CPU_PG" --memory="$PG_MEM" \
    --tmpfs "/var/lib/postgresql/data:size=$disk" \
    -e POSTGRES_USER=shortener \
    -e POSTGRES_PASSWORD=shortener \
    -e POSTGRES_DB=shortener \
    "$PG" \
    postgres -c shared_buffers=256MB -c max_connections=20 -c work_mem=4MB $PG_ARGS_EXTRA >/dev/null

  docker run -d --rm --name sb_redis --network "$NET" \
    --cpus="$CPU_REDIS" --memory="$REDIS_MEM" \
    "$REDIS" \
    redis-server --maxmemory 192mb --maxmemory-policy volatile-ttl >/dev/null

  # Wait for Postgres to accept connections rather than sleeping blindly.
  for _ in $(seq 1 60); do
    if docker exec sb_pg pg_isready -U shortener -d shortener >/dev/null 2>&1; then
      break
    fi
    sleep 1
  done
  docker exec sb_pg pg_isready -U shortener -d shortener >/dev/null
}

cmd_verify() {
  echo "Verifying the sandbox imposes the target hardware limits."
  echo "Target: ${TARGET_CPUS} vCPU, ${TARGET_MEM} RAM"
  echo

  start_services "${DISK_FUNCTIONAL}"

  # 1. CPU quota.
  #    NOT `nproc`: that reads CPU affinity, which --cpus does not touch.
  #    --cpus sets a CFS bandwidth quota, readable only from cgroup v2.
  echo "1. CPU quota (expect ${TARGET_CPUS}):"
  local cpu_max
  cpu_max="$(docker run --rm --cpus="$TARGET_CPUS" --memory="$TARGET_MEM" "$BUILDER" \
    sh -c 'cat /sys/fs/cgroup/cpu.max' | tr -d '\r')"
  echo "   /sys/fs/cgroup/cpu.max = ${cpu_max}"
  if [ "$cpu_max" != "100000 100000" ]; then
    echo "   UNEXPECTED: quota is not 1 CPU worth (100000/100000)"
    teardown; return 1
  fi
  echo "   host has $(nproc) cores; nproc inside reports $(docker run --rm --cpus="$TARGET_CPUS" "$BUILDER" sh -c nproc | tr -d '\r')"
  echo "   -> nproc is NOT the proof, it reads affinity not quota"

  # 2. Memory quota, likewise from cgroup v2 rather than /proc/meminfo.
  echo "2. Memory quota (expect ${TARGET_MEM}):"
  local mem_max
  mem_max="$(docker run --rm --cpus="$TARGET_CPUS" --memory="$TARGET_MEM" "$BUILDER" \
    sh -c 'cat /sys/fs/cgroup/memory.max' | tr -d '\r')"
  echo "   /sys/fs/cgroup/memory.max = ${mem_max} bytes (=$(( mem_max / 1024 / 1024 )) MB)"
  if [ "$mem_max" != "4294967296" ]; then
    echo "   UNEXPECTED: memory.max is not 4 GiB"
    teardown; return 1
  fi

  # 3. The memory cap actually bites: allocate past it and confirm failure.
  echo -n "3. Memory cap bites: allocating 6 GB in a ${TARGET_MEM} container -> "
  if docker run --rm --cpus="$TARGET_CPUS" --memory="$TARGET_MEM" "$BUILDER" \
    sh -c 'dd if=/dev/zero of=/dev/shm/big bs=1M count=6144 2>/dev/null' >/dev/null 2>&1; then
    echo "UNEXPECTED: 6 GB allocation succeeded"
    teardown; return 1
  else
    echo "allocation refused as expected"
  fi

  # 4. The CPU quota actually bites: two spinning threads must be throttled.
  echo -n "4. CPU quota bites: two spinning threads should show throttling -> "
  local throttled
  throttled="$(docker run --rm --cpus="$TARGET_CPUS" --memory="$TARGET_MEM" "$BUILDER" sh -c '
      (while :; do :; done) & (while :; do :; done) & sleep 3; kill %1 %2 2>/dev/null
      grep nr_throttled /sys/fs/cgroup/cpu.stat | awk "{print \$2}"' 2>/dev/null | tr -d '\r' | tail -1)"
  echo "nr_throttled=${throttled}"
  if [ -z "$throttled" ] || [ "$throttled" = "0" ]; then
    echo "   UNEXPECTED: no throttling observed, the quota is not being enforced"
    teardown; return 1
  fi

  # 5. Disk cap on the Postgres tmpfs, then prove a full disk fails writes.
  echo "5. Disk cap on Postgres tmpfs (expect ${DISK_FUNCTIONAL}):"
  local disk_avail
  disk_avail="$(docker exec sb_pg sh -c "df -h /var/lib/postgresql/data | awk 'NR==2{print \$2}'" | tr -d '\r')"
  echo "   available = ${disk_avail} (host / has $(df -h / | awk 'NR==2{print $2}') free)"
  echo -n "   writing 4 GB into it should fail -> "
  if docker exec sb_pg sh -c \
      'dd if=/dev/zero of=/var/lib/postgresql/data/fill bs=1M count=4096 2>/dev/null' >/dev/null 2>&1; then
    echo "UNEXPECTED: 4 GB write succeeded inside a ${DISK_FUNCTIONAL} tmpfs"
    docker exec sb_pg rm -f /var/lib/postgresql/data/fill >/dev/null 2>&1 || true
    teardown; return 1
  else
    echo "write failed as expected (No space left on device)"
  fi

  docker exec sb_pg rm -f /var/lib/postgresql/data/fill >/dev/null 2>&1 || true
  echo
  echo "All limits verified, and each one was observed to bite."
  teardown
}

run_in_builder() {
  local disk="$1"; shift
  local extra_env=("$@")

  start_services "$disk"

  local -a env_args=()
  for e in "${extra_env[@]:-}"; do
    [ -n "$e" ] && env_args+=(-e "$e")
  done

  # Tell the tests what limits to expect, so test_build.c can prove the running
  # container matches the declaration instead of trusting it.
  local cpu_quota_bytes mem_bytes
  cpu_quota_bytes=$(awk -v c="$CPU_APP" 'BEGIN{printf "%d", c*100000}')
  mem_bytes=$(awk -v m="$APP_MEM" 'BEGIN{
      if (m ~ /g$/) printf "%d", m*1024*1024*1024
      else if (m ~ /m$/) printf "%d", m*1024*1024
      else printf "%d", m*1024*1024 }')

  docker run --rm --network "$NET" --cpus="$CPU_APP" --memory="$APP_MEM" \
    -v "$ROOT":/src:ro \
    "${env_args[@]}" \
    -e TEST_DATABASE_URL="postgresql://shortener:shortener@sb_pg:5432/shortener" \
    -e TEST_REDIS_URL="redis://sb_redis:6379/0" \
    -e SANDBOX_EXPECT_CPU_QUOTA="$cpu_quota_bytes" \
    -e SANDBOX_EXPECT_MEM_BYTES="$mem_bytes" \
    -e SANDBOX_DISK_BYTES="$(disk_bytes "$disk")" \
    -e SANDBOX_HOST_CPUS="$(nproc)" \
    "$BUILDER" sh -c "
      $INSTALL_DEPS
      cp -r /src /build && cd /build
      $MAKE_CMD
    "
  local rc=$?
  teardown
  return $rc
}

cmd_test() {
  MAKE_CMD="make test" run_in_builder "$DISK_FUNCTIONAL"
}

cmd_test_disk() {
  echo "Running the full ${DISK_FULL} disk profile."
  MAKE_CMD="make test-disk" run_in_builder "$DISK_FULL"
}

# Fills Postgres until it refuses to write, against a tmpfs small enough to get
# there in under a minute.
cmd_test_diskfull() {
  echo "Running the ${DISK_FULL_TEST} disk-full profile."
  PG_ARGS_EXTRA="$PG_ARGS_DISK_TEST"
  MAKE_CMD="make test-disk" run_in_builder "$DISK_FULL_TEST"
}

# Task 1 requires a leak check; Task 2 requires a sanitizer build. Valgrind is
# not in Debian trixie-slim's default set, so it is installed here rather than
# assumed. ASan needs the same libs.
cmd_test_asan() {
  start_services "$DISK_FUNCTIONAL"

  local cpu_quota_bytes mem_bytes
  cpu_quota_bytes=$(awk -v c="$CPU_APP" 'BEGIN{printf "%d", c*100000}')
  mem_bytes=$(awk -v m="$APP_MEM" 'BEGIN{
      if (m ~ /g$/) printf "%d", m*1024*1024*1024
      else if (m ~ /m$/) printf "%d", m*1024*1024
      else printf "%d", m*1024*1024 }')

  set +e
  docker run --rm --network "$NET" --cpus="$CPU_APP" --memory="$APP_MEM" \
    -v "$ROOT":/src:ro \
    -e TEST_DATABASE_URL="postgresql://shortener:shortener@sb_pg:5432/shortener" \
    -e TEST_REDIS_URL="redis://sb_redis:6379/0" \
    -e SANDBOX_EXPECT_CPU_QUOTA="$cpu_quota_bytes" \
    -e SANDBOX_EXPECT_MEM_BYTES="$mem_bytes" \
    -e SANDBOX_HOST_CPUS="$(nproc)" \
    "$BUILDER" sh -c "
      export DEBIAN_FRONTEND=noninteractive
      apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        gcc make libcriterion-dev libpq-dev libhiredis-dev pkg-config \
        valgrind ca-certificates >/dev/null 2>&1
      cp -r /src /build && cd /build
      echo '--- ASan + UBSan ---'
      UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 make test-asan
      asan_rc=\$?
      echo
      echo '--- Valgrind leak check on the plain test binaries ---'
      vg_rc=0
      make test >/dev/null 2>&1 || true
      found=0
      for t in build/test_*; do
        [ -f \"\$t\" ] || continue
        # Fills the database on purpose; it belongs to the disk-full profile,
        # and running it here would make every later valgrind pass meaningless.
        case \"\$t\" in *test_disk_full) continue ;; esac
        found=1
        echo \"=== valgrind \$t\"
        valgrind --leak-check=full --errors-for-leak-kinds=definite \
                 --error-exitcode=42 --quiet \"\$t\" 2>&1 | tail -20 || vg_rc=1
      done
      [ \$found -eq 1 ] || { echo 'no test binaries built, valgrind pass is vacuous'; vg_rc=1; }
      [ \$asan_rc -eq 0 ] && [ \$vg_rc -eq 0 ]
    "
  local rc=$?
  set -e
  teardown
  return $rc
}

cmd_e2e() {
  MAKE_CMD="make test-e2e" run_in_builder "$DISK_FUNCTIONAL"
}

cmd_bench() {
  local duration="${1:-60}"
  start_services "$DISK_FUNCTIONAL"
  docker build -q -t shortener:test . >/dev/null

  # Server in its own container so the generator drives it over the network,
  # sharing the same capped core.
  docker run -d --rm --name sb_app --network "$NET" \
    --cpus="$CPU_APP" --memory="$APP_MEM" \
    -e DATABASE_URL="postgresql://shortener:shortener@sb_pg:5432/shortener" \
    -e REDIS_URL="redis://sb_redis:6379/0" \
    shortener:test >/dev/null

  sleep 2
  set +e
  docker exec sb_app ./bin/load_test --host 127.0.0.1 --threads 2 --duration "$duration"
  local rc=$?
  set -e
  teardown
  return $rc
}

cmd_teardown() {
  teardown
  # The `|| true` matters: grep exits 1 when nothing matches, and under
  # `set -e` that would kill the script before it prints the verdict.
  local leftovers
  leftovers="$(docker ps -a --filter 'name=sb_' --format '{{.Names}}' || true)"
  if [ -n "$leftovers" ]; then
    echo "leftover containers:"
    echo "$leftovers" | sed 's/^/  /'
  fi
  if docker network ls --format '{{.Name}}' | grep -qx "$NET"; then
    echo "leftover network: $NET"
  fi
  echo "teardown complete"
}

case "${1:-}" in
  verify)    cmd_verify ;;
  test)      cmd_test ;;
  test-disk) cmd_test_disk ;;
  test-diskfull) cmd_test_diskfull ;;
  test-asan) cmd_test_asan ;;
  e2e)       cmd_e2e ;;
  bench)     shift; cmd_bench "$@" ;;
  teardown)  cmd_teardown ;;
  *)
    sed -n '2,20p' "$0"
    exit 1
    ;;
esac
