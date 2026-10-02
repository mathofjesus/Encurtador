/* Tests for the retention job.
 *
 * The point of this job is not the delete; Postgres does that on its own. The
 * point is that the cache is a second copy of the data, so deleting the row is
 * only half the work. These tests warm the cache, expire a row underneath it,
 * and assert that the sweep removes the row and the entry together while
 * leaving a live URL and its entry alone.
 *
 * The first test drives application_cleanup, the same entry point the --cleanup
 * mode uses, so the invalidation wiring is covered and not just the loop. The
 * rest drive cleanup_run directly, where the batching and the callback contract
 * can be observed.
 *
 * Real Postgres and real Redis: a fake cache would turn the invalidation this
 * suite is about into a mock's bookkeeping.
 */
#include <criterion/criterion.h>
#include <libpq-fe.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "api/application.h"
#include "cache/redis.h"
#include "codegen.h"
#include "db/pg.h"
#include "jobs/cleanup.h"
#include "services/url_service.h"

static const char *test_dsn(void)
{
    const char *dsn = getenv("TEST_DATABASE_URL");
    if (!dsn || !*dsn)
        dsn = getenv("DATABASE_URL");
    return dsn;
}

static const char *test_redis_url(void)
{
    const char *url = getenv("TEST_REDIS_URL");
    if (!url || !*url)
        url = getenv("REDIS_URL");
    return url;
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

static redis_client_t *fresh_cache(void)
{
    const char *url = test_redis_url();
    cr_assert_not_null(url, "neither TEST_REDIS_URL nor REDIS_URL is set");

    redis_client_t *c = redis_init(url);
    cr_assert_not_null(c);
    return c;
}

/* application_new needs a whole config. A stack one built from the sandbox's
 * environment keeps the test off the config file and its env overrides. */
static void fill_config(config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    const char *dsn = test_dsn();
    cr_assert_not_null(dsn, "neither TEST_DATABASE_URL nor DATABASE_URL is set");
    snprintf(cfg->database.primary_dsn, sizeof(cfg->database.primary_dsn), "%s",
             dsn);
    cfg->database.pool_size = 2;

    const char *redis = test_redis_url();
    if (redis && *redis)
        snprintf(cfg->cache.redis_url, sizeof(cfg->cache.redis_url), "%s", redis);
    cfg->cache.redis_ttl_seconds = 3600;

    snprintf(cfg->app.base_url, sizeof(cfg->app.base_url), "http://localhost:8000");
    cfg->rate_limit.per_minute = 60;
    cfg->rate_limit.burst = 10;
    cfg->retention.default_ttl_days = 30;
    cfg->retention.max_ttl_days = 3650;
    cfg->codegen.min_length = CODE_MIN_LENGTH;
    cfg->codegen.max_length = CODE_MAX_LENGTH;
}

/* Removes rows whose code starts with "test", so runs do not collide. */
static void wipe_codes(void)
{
    PGconn *c = PQconnectdb(test_dsn());
    cr_assert_eq(PQstatus(c), CONNECTION_OK, "connect failed: %s", PQerrorMessage(c));
    PGresult *r = PQexec(c, "DELETE FROM urls WHERE code LIKE 'test%'");
    cr_assert(PQresultStatus(r) == PGRES_COMMAND_OK, "wipe failed: %s",
              PQerrorMessage(c));
    PQclear(r);
    PQfinish(c);
}

static void key_for(const char *code, char *out, size_t out_len)
{
    snprintf(out, out_len, "url:%s", code);
}

static void warm(redis_client_t *cache, const char *code, const char *value)
{
    char key[32];
    key_for(code, key, sizeof(key));
    cr_assert_eq(redis_setex(cache, key, value, 3600), REDIS_OK,
                 "could not warm the cache for %s", code);
}

static int cached(redis_client_t *cache, const char *code)
{
    char key[32];
    key_for(code, key, sizeof(key));
    char value[512];
    return redis_get(cache, key, value, sizeof(value)) == REDIS_OK;
}

/* Copies a code, because cleanup_run promises the pointer is only valid for the
 * call, and recording it is how that promise is checked. */
typedef struct {
    int calls;
    char codes[8][16];
    char last[16];
} recorder_t;

static void record(void *user_data, const char *code)
{
    recorder_t *r = user_data;
    if (r->calls < (int)(sizeof(r->codes) / sizeof(r->codes[0])))
        snprintf(r->codes[r->calls], sizeof(r->codes[0]), "%s", code);
    snprintf(r->last, sizeof(r->last), "%s", code);
    r->calls++;
}

Test(cleanup, the_job_deletes_the_expired_row_and_its_cache_entry)
{
    config_t cfg;
    fill_config(&cfg);
    application_t *app = application_new(&cfg, NULL);
    cr_assert_not_null(app, "application_new failed (is TEST_DATABASE_URL set?)");

    /* A second pool for the fixtures: application owns its own and does not
     * expose it, and inserting the rows through the thing under test would make
     * the setup part of the assertion. */
    pg_pool_t *pool = fresh_pool();
    redis_client_t *cache = fresh_cache();
    wipe_codes();

    time_t now = time(NULL);
    cr_assert_eq(pg_create_url(pool, "testexp01", "https://example.com/expired",
                               now - 3600), PG_OK);
    cr_assert_eq(pg_create_url(pool, "testlive1", "https://example.com/alive",
                               now + 86400), PG_OK);

    /* Warm both, including the expired one. url_service_lookup would refuse to
     * cache an already-expired row, so writing it directly is the only way to
     * reproduce the entry that outlives its row. */
    warm(cache, "testexp01", "https://example.com/expired");
    warm(cache, "testlive1", "https://example.com/alive");
    cr_assert(cached(cache, "testexp01"));
    cr_assert(cached(cache, "testlive1"));

    long deleted = -1;
    cr_assert_eq(application_cleanup(app, now, 100, &deleted), PG_OK);
    cr_assert_eq(deleted, 1, "only the expired row should be collected");

    char url[PG_MAX_URL + 1];
    time_t expires = 0;
    cr_assert_eq(pg_lookup_url(pool, "testexp01", url, sizeof(url), &expires),
                 PG_NOT_FOUND, "the expired row must be gone");
    cr_assert(!cached(cache, "testexp01"),
              "the cached entry outlived its row: the invalidation was skipped");

    /* The live URL and its entry must be untouched. */
    cr_assert_eq(pg_lookup_url(pool, "testlive1", url, sizeof(url), &expires),
                 PG_OK, "a live row must survive the sweep");
    cr_assert(cached(cache, "testlive1"), "a live entry must not be invalidated");

    wipe_codes();
    application_free(app);
    redis_free(cache);
    pg_pool_free(pool);
}

Test(cleanup, a_short_batch_still_drains_every_expired_row)
{
    pg_pool_t *pool = fresh_pool();
    wipe_codes();

    time_t now = time(NULL);
    for (int i = 0; i < 3; i++) {
        char code[16];
        snprintf(code, sizeof(code), "testbtch%d", i);
        cr_assert_eq(pg_create_url(pool, code, "https://example.com/dead", now - 60),
                     PG_OK);
    }

    /* batch_size 1 forces the loop: every batch is full, so only an extra empty
     * query stops it. A job that stopped after the first batch would leave two
     * rows and two stale entries. */
    recorder_t rec = {0};
    long deleted = 0;
    cr_assert_eq(cleanup_run(pool, now, 1, record, &rec, &deleted), PG_OK);
    cr_assert_eq(deleted, 3, "the loop must drain every batch");
    cr_assert_eq(rec.calls, 3, "each drained code must be invalidated exactly once");

    char url[PG_MAX_URL + 1];
    time_t expires = 0;
    for (int i = 0; i < 3; i++) {
        char code[16];
        snprintf(code, sizeof(code), "testbtch%d", i);
        cr_assert_eq(pg_lookup_url(pool, code, url, sizeof(url), &expires),
                     PG_NOT_FOUND, "%s survived a full sweep", code);
    }

    wipe_codes();
    pg_pool_free(pool);
}

Test(cleanup, a_missing_callback_is_allowed)
{
    pg_pool_t *pool = fresh_pool();
    wipe_codes();

    time_t now = time(NULL);
    cr_assert_eq(pg_create_url(pool, "testnocb1", "https://example.com/dead",
                               now - 60), PG_OK);

    long deleted = 0;
    cr_assert_eq(cleanup_run(pool, now, 100, NULL, NULL, &deleted), PG_OK);
    cr_assert_eq(deleted, 1);

    wipe_codes();
    pg_pool_free(pool);
}

Test(cleanup, url_service_invalidate_drops_one_entry_and_leaves_another)
{
    pg_pool_t *pool = fresh_pool();
    redis_client_t *cache = fresh_cache();
    wipe_codes();

    config_t cfg;
    fill_config(&cfg);
    url_service_t *svc = url_service_new(pool, cache, &cfg, NULL);
    cr_assert_not_null(svc);

    warm(cache, "testinv01", "https://example.com/one");
    warm(cache, "testinv02", "https://example.com/two");

    url_service_invalidate(svc, "testinv01");
    cr_assert(!cached(cache, "testinv01"), "the named entry must be dropped");
    cr_assert(cached(cache, "testinv02"), "an unrelated entry must be kept");

    /* Safe on the edges, so a cleanup callback cannot fault on a stray code. */
    url_service_invalidate(svc, NULL);
    url_service_invalidate(svc, "");
    url_service_invalidate(NULL, "testinv02");
    cr_assert(cached(cache, "testinv02"));

    url_service_free(svc);
    redis_free(cache);
    pg_pool_free(pool);
}
