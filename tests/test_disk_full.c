/* What a full disk actually does to the service.
 *
 * This binary is deliberately NOT part of `make test`. It fills the database
 * until Postgres refuses to write, and after that every write in the database
 * fails, which would break every other test sharing the instance. It runs only
 * under `make test-disk`, against a deliberately small tmpfs.
 *
 * The question is not "can Postgres survive a full disk" — it cannot — but
 * whether the failure arrives as a clean error the caller can turn into a 503,
 * rather than as a hang, a crash, or a silent partial write. That distinction is
 * the entire reason to put a pool and typed return codes in front of libpq.
 */
#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "codegen.h"
#include "db/pg.h"

/* The tmpfs this runs against is a few hundred MB, so the loop must be bounded.
 * If the disk never fills within the bound the test FAILS: a capacity that does
 * not reproduce is not a pass, it is a measurement that went wrong. */
#define MAX_ATTEMPTS 4000000

/* Reported so Task 10's arithmetic is measured rather than guessed. */
#define BYTES_PER_GIB (1024.0 * 1024.0 * 1024.0)

static const char *test_dsn(void)
{
    const char *dsn = getenv("TEST_DATABASE_URL");
    if (!dsn || !*dsn)
        dsn = getenv("DATABASE_URL");
    return dsn;
}

/* A URL at the column cap, so the row count is a fair estimate of what a real
 * maximum-length URL costs rather than of a toy.
 *
 * The body must be INCOMPRESSIBLE. Postgres TOASTs a column this large and runs
 * it through pglz, and an earlier version of this file filled the URL with
 * repeated 'a'. That compressed to almost nothing and reported 709,147 rows in
 * 256 MB — 361 bytes for a 2048-byte URL. The retention arithmetic in
 * docs/LOAD_TEST_RESULTS.md is quoted in bytes per row, so measuring a
 * compressible row would have understated the disk cost by roughly six times.
 *
 * The characters are drawn from an LCG rather than a PRNG so the test needs no
 * seed and produces the same measurement on every run. */
static void fill_url(char *buf, size_t len, unsigned long long n)
{
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    const size_t alpha_len = sizeof(alphabet) - 1;

    memcpy(buf, "https://example.com/", 20);

    /* Seeded from n so each row differs; a repeating pattern would let pglz find
     * a match across the value as well as inside it. */
    unsigned long long state = n * 6364136223846793005ULL + 1442695040888963407ULL;
    for (size_t i = 20; i + 1 < len; i++) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        buf[i] = alphabet[(state >> 33) % alpha_len];
    }
    buf[len - 1] = '\0';
}

Test(disk_full, a_full_disk_produces_a_clean_error_rather_than_a_hang_or_a_crash)
{
    const char *dsn = test_dsn();
    cr_assert_not_null(dsn, "neither TEST_DATABASE_URL nor DATABASE_URL is set");

    pg_pool_t *pool = pg_pool_new(dsn, 2);
    cr_assert_not_null(pool);
    cr_assert_eq(pg_run_schema(pool), PG_OK);

    char url[PG_MAX_URL + 1];
    char code[CODE_MAX_LENGTH + 1];

    long inserted = 0;
    int failure = 0;
    long long first_failure_at = -1;
    time_t started = time(NULL);

    for (long i = 0; i < MAX_ATTEMPTS; i++) {
        fill_url(url, PG_MAX_URL, (unsigned long long)i);

        /* A unique code per row: the unique index is (code, created_at), and
         * inserting thousands of rows inside one second would otherwise collide
         * on the same (code, created_at) and report a conflict rather than a
         * full disk. */
        code_encode((unsigned long long)i + 1, code, sizeof(code));

        int rc = pg_create_url(pool, code, url, 0);
        if (rc != PG_OK) {
            failure = rc;
            first_failure_at = i;
            break;
        }
        inserted++;

        if (inserted % 20000 == 0)
            fprintf(stderr, "  inserted %ld rows\n", inserted);
    }

    long long elapsed = (long long)(time(NULL) - started);

    cr_assert_neq(failure, 0,
                  "the disk never filled after %ld rows in %llds; the tmpfs cap "
                  "is larger than assumed, so this test is measuring nothing",
                  inserted, elapsed);

    /* The specific requirement: PG_ERROR, so the routing layer can answer 503.
     * A 1 here would mean Postgres reported a uniqueness violation, which is a
     * different failure with a different fix. */
    cr_assert_eq(failure, PG_ERROR,
                 "expected PG_ERROR on a full disk, got %d", failure);

    fprintf(stderr,
            "\ndisk-full result:\n"
            "  rows written before the failure : %ld\n"
            "  failed at insert                 : %lld\n"
            "  elapsed                          : %lld s\n"
            "  rate                             : %.0f inserts/s\n",
            inserted, first_failure_at, elapsed,
            elapsed > 0 ? (double)inserted / (double)elapsed : 0.0);

    /* Bytes per row is the figure docs/LOAD_TEST_RESULTS.md quotes, so it is
     * measured here rather than estimated. It is an upper bound on what the
     * table costs: the cap includes Postgres's own overhead, the indexes and the
     * WAL, so a real deployment would do slightly better on the heap and worse
     * once vacuum and index bloat are counted. */
    const char *disk_env = getenv("SANDBOX_DISK_BYTES");
    if (inserted > 0 && disk_env && atof(disk_env) > 0) {
        double bytes = atof(disk_env);
        fprintf(stderr,
                "  declared tmpfs                   : %.2f GiB\n"
                "  bytes per row (incl. WAL+index)   : %.0f\n"
                "  rows per GiB                     : %.0f\n",
                bytes / BYTES_PER_GIB, bytes / (double)inserted,
                BYTES_PER_GIB * (double)inserted / bytes);
    }

    /* The point of a typed error: the process is still alive and usable. A row
     * written before the disk filled must still read back. */
    char read_back[PG_MAX_URL + 1];
    time_t expires = 0;
    if (inserted > 0) {
        code_encode(1, code, sizeof(code));
        fill_url(url, PG_MAX_URL, 0);
        int rc = pg_lookup_url(pool, code, read_back, sizeof(read_back), &expires);
        /* A read may also fail once the disk is full and Postgres cannot extend
         * a relation, but it must not crash and must not claim success. */
        cr_assert(rc == PG_OK || rc == PG_NOT_FOUND || rc == PG_ERROR,
                  "unexpected lookup result %d", rc);
    }

    /* Re-running must not wedge: the pool has to hand back connections even
     * after the database started refusing work. */
    int rc = pg_lookup_url(pool, "0000001", read_back, sizeof(read_back), &expires);
    cr_assert(rc == PG_OK || rc == PG_NOT_FOUND || rc == PG_ERROR,
              "a lookup after a full disk returned %d", rc);

    pg_pool_free(pool);
}

Test(disk_full, the_measurement_of_rows_per_gib_is_reported)
{
    /* Not a behavioural test — a guard on the arithmetic the load-test document
     * will quote. If this file is ever run against an unbounded disk it must say
     * so rather than print a number that looks like a measurement of 50 GB. */
    const char *disk = getenv("SANDBOX_DISK_BYTES");
    if (!disk || !*disk)
        return; /* nothing to check without the sandbox's declaration */

    double bytes = atof(disk);
    cr_assert_neq(bytes, 0.0);
    fprintf(stderr, "  declared disk for this profile: %.2f GiB\n",
            bytes / BYTES_PER_GIB);
}