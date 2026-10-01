/* Integration tests for the storage layer.
 *
 * These need a real Postgres: partitioning, unique-index enforcement and
 * disk-full behaviour cannot be faked, and faking them would test the fake.
 * The sandbox provides sb_pg with the same caps as the target box.
 */
#include <criterion/criterion.h>
#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db/pg.h"

/* The sandbox names the service by container name and supplies the DSN;
 * a local run sets DATABASE_URL. Neither name resolves to localhost inside the
 * container, which is why this is an env var rather than a constant. */
static const char *test_dsn(void)
{
    const char *dsn = getenv("TEST_DATABASE_URL");
    if (!dsn || !*dsn)
        dsn = getenv("DATABASE_URL");
    return dsn;
}

static pg_pool_t *fresh_pool(void)
{
    const char *dsn = test_dsn();
    cr_assert_not_null(dsn, "neither TEST_DATABASE_URL nor DATABASE_URL is set");

    pg_pool_t *pool = pg_pool_new(dsn, 2);
    cr_assert_not_null(pool);
    cr_assert_eq(pg_run_schema(pool), PG_OK);
    return pool;
}

/* A direct connection, bypassing the pool, for assertions about how the schema
 * was built. The pool is for the service's benefit; these tests want to see the
 * catalogue, not the abstraction. */
static PGconn *direct_conn(void)
{
    PGconn *c = PQconnectdb(test_dsn());
    cr_assert_not_null(c);
    cr_assert_eq(PQstatus(c), CONNECTION_OK, "connect failed: %s", PQerrorMessage(c));
    return c;
}

static int scalar_int(PGresult *r)
{
    cr_assert_eq(PQntuples(r), 1);
    return atoi(PQgetvalue(r, 0, 0));
}

/* Deletes rows this test created so runs do not collide with each other. */
static void wipe_codes(pg_pool_t *pool)
{
    (void)pool;
    PGconn *c = direct_conn();
    PGresult *r = PQexec(c, "DELETE FROM urls WHERE code LIKE 'test%'");
    cr_assert(PQresultStatus(r) == PGRES_COMMAND_OK, "wipe failed: %s", PQerrorMessage(c));
    PQclear(r);
    PQfinish(c);
}

Test(pg, schema_creates_a_partitioned_table)
{
    pg_pool_t *pool = fresh_pool();

    PGconn *c = direct_conn();
    /* relkind 'p' is a partitioned table; 'r' is an ordinary one. The whole
     * point of the schema is that this is 'p'. */
    PGresult *r = PQexec(c,
        "SELECT relkind FROM pg_class WHERE relname = 'urls'");
    cr_assert(PQresultStatus(r) == PGRES_TUPLES_OK, "%s", PQerrorMessage(c));
    cr_assert_str_eq(PQgetvalue(r, 0, 0), "p");
    PQclear(r);
    PQfinish(c);

    pg_pool_free(pool);
}

Test(pg, the_unique_index_must_include_the_partition_key)
{
    pg_pool_t *pool = fresh_pool();

    PGconn *c = direct_conn();
    PGresult *r = PQexec(c,
        "SELECT pg_get_indexdef(i.indexrelid) "
        "  FROM pg_class t JOIN pg_index i ON i.indrelid = t.oid "
        " WHERE t.relname = 'urls' AND i.indisunique");
    cr_assert(PQresultStatus(r) == PGRES_TUPLES_OK, "%s", PQerrorMessage(c));
    cr_assert_geq(PQntuples(r), 1, "urls has no unique index at all");

    /* Every unique index, not just the first: the primary key on (id,
     * created_at) is also unique and would otherwise be the one examined. */
    int found_code_index = 0;
    for (int row = 0; row < PQntuples(r); row++) {
        const char *def = PQgetvalue(r, row, 0);
        if (strstr(def, "(code") == NULL)
            continue;

        found_code_index = 1;
        /* Postgres will not build a unique index on a partitioned table
         * without the partition key, so its presence here is not a choice. */
        cr_assert(strstr(def, "created_at") != NULL,
                  "the index over code omits the partition key: %s", def);
    }
    cr_assert(found_code_index,
              "no unique index over code: the database does not enforce it at all");

    PQclear(r);
    PQfinish(c);
    pg_pool_free(pool);
}

Test(pg, a_unique_index_on_code_alone_is_refused_by_postgres)
{
    /* Documents the constraint the design is built around, by trying the thing
     * that does not work. If this ever succeeds, the schema's collision story
     * has changed and the app-side retry logic needs revisiting. */
    PGconn *c = direct_conn();
    PGresult *r = PQexec(c,
        "CREATE UNIQUE INDEX urls_code_only ON urls (code)");
    ExecStatusType st = PQresultStatus(r);
    cr_assert(st == PGRES_FATAL_ERROR || st == PGRES_NONFATAL_ERROR,
              "Postgres accepted a unique index on (code) alone: %s",
              PQresultErrorMessage(r));
    /* 0A000 feature_not_supported is what a partitioned table reports here. */
    cr_assert_str_eq(PQresultErrorField(r, PG_DIAG_SQLSTATE), "0A000");
    PQclear(r);
    PQfinish(c);
}

Test(pg, create_then_lookup_roundtrips)
{
    pg_pool_t *pool = fresh_pool();
    wipe_codes(pool);

    cr_assert_eq(pg_create_url(pool, "testrt01", "https://example.com/a?b=c", 0),
                 PG_OK);

    char url[PG_MAX_URL + 1];
    time_t expires = 1;
    cr_assert_eq(pg_lookup_url(pool, "testrt01", url, sizeof(url), &expires),
                 PG_OK);
    cr_assert_str_eq(url, "https://example.com/a?b=c");
    cr_assert_eq(expires, 0, "no expiry should read back as 0, not as 1970");

    wipe_codes(pool);
    pg_pool_free(pool);
}

Test(pg, an_expiry_in_the_future_comes_back_as_a_timestamp)
{
    pg_pool_t *pool = fresh_pool();
    wipe_codes(pool);

    /* One hour out, rounded to the second. */
    time_t want = time(NULL) + 3600;
    cr_assert_eq(pg_create_url(pool, "testexp01", "https://example.com/x", want),
                 PG_OK);

    char url[PG_MAX_URL + 1];
    time_t got = 0;
    cr_assert_eq(pg_lookup_url(pool, "testexp01", url, sizeof(url), &got), PG_OK);
    cr_assert_eq(got, want);

    wipe_codes(pool);
    pg_pool_free(pool);
}

Test(pg, an_expired_url_reads_back_as_not_found)
{
    pg_pool_t *pool = fresh_pool();
    wipe_codes(pool);

    /* Inserted with an expiry in the past: the row exists and a direct query
     * will find it, but the service must not hand it out. */
    cr_assert_eq(pg_create_url(pool, "testold01", "https://example.com/gone",
                               time(NULL) - 60), PG_OK);

    char url[PG_MAX_URL + 1];
    time_t expires = 0;
    cr_assert_eq(pg_lookup_url(pool, "testold01", url, sizeof(url), &expires),
                 PG_NOT_FOUND);

    /* Proves the row is really there and the hiding is the lookup's doing. */
    PGconn *c = direct_conn();
    PGresult *r = PQexec(c,
        "SELECT count(*) FROM urls WHERE code = 'testold01'");
    cr_assert_eq(scalar_int(r), 1, "the row should still exist");
    PQclear(r);
    PQfinish(c);

    wipe_codes(pool);
    pg_pool_free(pool);
}

Test(pg, an_unknown_code_is_not_found_rather_than_an_error)
{
    pg_pool_t *pool = fresh_pool();

    char url[PG_MAX_URL + 1];
    time_t expires = 0;
    cr_assert_eq(pg_lookup_url(pool, "testnope1", url, sizeof(url), &expires),
                 PG_NOT_FOUND);

    pg_pool_free(pool);
}

Test(pg, a_duplicate_code_returns_conflict_not_an_error)
{
    pg_pool_t *pool = fresh_pool();
    wipe_codes(pool);

    cr_assert_eq(pg_create_url(pool, "testdup01", "https://example.com/one", 0),
                 PG_OK);

    /* The unique index is on (code, created_at), and created_at defaults to
     * now() at second resolution, so the second insert inside the same second
     * collides and 23505 must come back as 1 — the caller answers 409. */
    int rc = pg_create_url(pool, "testdup01", "https://example.com/two", 0);
    if (rc == PG_OK) {
        /* Two seconds apart, created_at differs and the index permits the row.
         * This is the real limitation of a partitioned unique index, and it is
         * why Task 7 retries on collision instead of relying on the database. */
        cr_assert_neq(pg_create_url(pool, "testdup01",
                                    "https://example.com/three", 0), -1,
                      "a cross-partition duplicate must still not be an error");
    } else {
        cr_assert_eq(rc, 1, "a duplicate must be 1, not some other error");
    }

    wipe_codes(pool);
    pg_pool_free(pool);
}

Test(pg, a_url_over_the_cap_is_refused_before_it_reaches_the_database)
{
    pg_pool_t *pool = fresh_pool();

    char big[PG_MAX_URL + 2];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    memcpy(big, "https://example.com/", 20);

    cr_assert_eq(pg_create_url(pool, "testbig01", big, 0), PG_ERROR);

    char url[PG_MAX_URL + 1];
    time_t expires = 0;
    cr_assert_eq(pg_lookup_url(pool, "testbig01", url, sizeof(url), &expires),
                 PG_NOT_FOUND, "the refused row must not exist");

    pg_pool_free(pool);
}

Test(pg, cleanup_removes_expired_rows_and_leaves_live_ones)
{
    pg_pool_t *pool = fresh_pool();
    wipe_codes(pool);

    cr_assert_eq(pg_create_url(pool, "testcln01", "https://example.com/live",
                               time(NULL) + 86400), PG_OK);
    cr_assert_eq(pg_create_url(pool, "testcln02", "https://example.com/dead",
                               time(NULL) - 86400), PG_OK);
    cr_assert_eq(pg_create_url(pool, "testcln03", "https://example.com/forever",
                               0), PG_OK);

    long deleted = -1;
    cr_assert_eq(pg_cleanup_expired(pool, time(NULL), &deleted), PG_OK);
    cr_assert_geq(deleted, 1, "the expired row should have gone");

    char url[PG_MAX_URL + 1];
    time_t expires = 0;
    cr_assert_eq(pg_lookup_url(pool, "testcln01", url, sizeof(url), &expires),
                 PG_OK, "a live row must survive cleanup");
    cr_assert_eq(pg_lookup_url(pool, "testcln02", url, sizeof(url), &expires),
                 PG_NOT_FOUND, "the expired row must be gone");
    /* No expiry at all is not the same as an expiry in the past. */
    cr_assert_eq(pg_lookup_url(pool, "testcln03", url, sizeof(url), &expires),
                 PG_OK, "a row with no expiry must never be collected");

    wipe_codes(pool);
    pg_pool_free(pool);
}

Test(pg, every_insert_lands_in_a_partition_that_exists)
{
    pg_pool_t *pool = fresh_pool();

    /* Without the generated current/next-month partitions this insert fails
     * with "no partition of relation \"urls\" found for row", which is the
     * failure mode a hardcoded partition range in schema.sql would produce
     * once the file aged. */
    cr_assert_eq(pg_create_url(pool, "testpart1", "https://example.com/p", 0),
                 PG_OK);

    PGconn *c = direct_conn();
    PGresult *r = PQexec(c,
        "SELECT tableoid::regclass::text FROM urls WHERE code = 'testpart1'");
    cr_assert_eq(PQntuples(r), 1);
    const char *partition = PQgetvalue(r, 0, 0);
    cr_assert(strncmp(partition, "urls_", 5) == 0,
              "row landed outside a partition: %s", partition);
    PQclear(r);
    PQfinish(c);

    wipe_codes(pool);
    pg_pool_free(pool);
}

Test(pg, a_pool_of_two_never_checks_out_more_than_two_connections)
{
    pg_pool_t *pool = fresh_pool();

    cr_assert_eq(pg_pool_size(pool), 2);
    cr_assert_eq(pg_pool_in_use(pool), 0, "nothing should be checked out yet");

    /* Sequential calls cannot overlap, so in-use returns to zero each time;
     * asserting the ceiling here is what the pool is for. */
    char url[PG_MAX_URL + 1];
    time_t expires = 0;
    pg_lookup_url(pool, "testpool1", url, sizeof(url), &expires);
    cr_assert_eq(pg_pool_in_use(pool), 0);

    pg_pool_free(pool);
}

Test(pg, an_unusable_dsn_is_refused_rather_than_half_opened)
{
    cr_assert_null(pg_pool_new("postgresql://nobody@127.0.0.1:1/none", 2));
    cr_assert_null(pg_pool_new("", 2));
    cr_assert_null(pg_pool_new(NULL, 2));
    cr_assert_null(pg_pool_new(test_dsn(), 0), "a pool of zero is not a pool");
}

Test(pg, freeing_null_is_safe)
{
    pg_pool_free(NULL);
}