/* Task 0: prove the sandbox imposes the hardware limits it claims.
 *
 * This is the first test in the project and it is not about the shortener. It
 * checks that the environment we measure in matches the environment the script
 * said it would create, because every capacity number in
 * docs/LOAD_TEST_RESULTS.md rests on that.
 *
 * scripts/sandbox.sh passes what it intended to impose:
 *   SANDBOX_EXPECT_CPU_QUOTA  first field of cpu.max, e.g. 35000
 *   SANDBOX_EXPECT_MEM_BYTES  memory.max in bytes, e.g. 1073741824
 *   SANDBOX_EXPECT_CPU_COUNT  the --cpus= value, e.g. 0.35
 *
 * Teaches: nproc reads CPU affinity, not the CFS quota that --cpus sets, and
 * /proc/meminfo is host-wide rather than cgroup-scoped. cgroup v2 is the only
 * trustworthy source. This test also proves the declaration and the reality
 * agree, so a misconfigured sandbox fails here rather than quietly producing
 * capacity numbers from the wrong machine.
 */

#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* First integer in a cgroup v2 file. Returns 0 on success. */
static int read_cgroup_long(const char *path, long *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int n = fscanf(f, "%ld", out);
    fclose(f);
    return n == 1 ? 0 : -1;
}

/* cpu.max is "quota period"; the quota is the field that matters. */
static int read_cpu_quota(long *quota)
{
    FILE *f = fopen("/sys/fs/cgroup/cpu.max", "r");
    if (!f) return -1;
    long q = -1, p = -1;
    int n = fscanf(f, "%ld %ld", &q, &p);
    fclose(f);
    if (n != 2) return -1;
    *quota = q;
    return 0;
}

/* cpu.stat is key/value per line; nr_throttled is not the first line. */
static int read_throttled_usec(long *out)
{
    FILE *f = fopen("/sys/fs/cgroup/cpu.stat", "r");
    if (!f) return -1;
    char key[64];
    long val = -1;
    while (fscanf(f, "%63s %ld", key, &val) == 2) {
        if (strcmp(key, "nr_throttled") == 0) {
            fclose(f);
            *out = val;
            return 0;
        }
    }
    fclose(f);
    return -1;
}

static long expect_quota(void)
{
    const char *v = getenv("SANDBOX_EXPECT_CPU_QUOTA");
    return v ? atol(v) : -1;
}

static long expect_mem(void)
{
    const char *v = getenv("SANDBOX_EXPECT_MEM_BYTES");
    return v ? atol(v) : -1;
}

/* Skip when not inside the sandbox: these assertions describe cgroup limits,
 * which mean nothing on a bare host. A silent pass here would be a lie. */
static int in_cgroup_v2(void)
{
    FILE *f = fopen("/sys/fs/cgroup/cpu.max", "r");
    if (!f) return 0;
    fclose(f);
    return getenv("SANDBOX_EXPECT_CPU_QUOTA") != NULL;
}

Test(sandbox, cpu_quota_matches_what_the_script_declared)
{
    if (!in_cgroup_v2())
        cr_skip_test("not running under scripts/sandbox.sh");

    long quota = -1;
    cr_assert_eq(read_cpu_quota(&quota), 0, "cannot read /sys/fs/cgroup/cpu.max");
    cr_assert_eq(quota, expect_quota(),
                 "cpu.max quota %ld does not match the declared %ld",
                 quota, expect_quota());
}

Test(sandbox, memory_quota_matches_what_the_script_declared)
{
    if (!in_cgroup_v2())
        cr_skip_test("not running under scripts/sandbox.sh");

    long max = 0;
    cr_assert_eq(read_cgroup_long("/sys/fs/cgroup/memory.max", &max), 0);
    cr_assert_eq(max, expect_mem(),
                 "memory.max %ld does not match the declared %ld",
                 max, expect_mem());
}

Test(sandbox, container_is_capped_below_the_host)
{
    if (!in_cgroup_v2())
        cr_skip_test("not running under scripts/sandbox.sh");

    /* The point of the sandbox: the app container gets a fraction of one core
     * while the host has many. If these ever matched, every benchmark in this
     * project would be measuring the host instead of the target machine. */
    const char *cc = getenv("SANDBOX_HOST_CPUS");
    if (!cc) cr_skip_test("SANDBOX_HOST_CPUS not provided");

    long quota = -1;
    cr_assert_eq(read_cpu_quota(&quota), 0);
    long host_cpus = atol(cc);
    cr_assert_geq(host_cpus, 2, "host unexpectedly has %ld cores", host_cpus);
    /* quota/period is the CPU allowance; 0.35 cores is 35000/100000. */
    cr_assert_lt(quota, 100000, "no CPU cap is in force at all");
}

Test(sandbox, nproc_is_not_evidence_of_a_cpu_cap)
{
    if (!in_cgroup_v2())
        cr_skip_test("not running under scripts/sandbox.sh");

    /* Documents the trap: nproc reports affinity, so it reports the host's core
     * count even under a fraction-of-a-core quota. Asserting nproc == 1 would
     * pass on the host for the wrong reason and fail in the sandbox for no
     * reason. The quota above is the real evidence. */
    long quota = -1;
    cr_assert_eq(read_cpu_quota(&quota), 0);
    cr_assert_lt(quota, 100000);
    cr_assert_geq(sysconf(_SC_NPROCESSORS_ONLN), 1);
}

Test(sandbox, throttling_counter_is_readable)
{
    if (!in_cgroup_v2())
        cr_skip_test("not running under scripts/sandbox.sh");

    long throttled = -1;
    cr_assert_eq(read_throttled_usec(&throttled), 0,
                 "nr_throttled not found in /sys/fs/cgroup/cpu.stat");
    /* Zero is legitimate for a suite that never exceeds its quota. Actual
     * throttling under load is proved by scripts/sandbox.sh verify, which
     * deliberately spins more threads than the quota allows. */
    cr_assert_geq(throttled, 0);
}
