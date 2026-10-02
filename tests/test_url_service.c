/* Integration tests for the URL service.
 *
 * These need the real Postgres and the real Redis: partitioning, the unique
 * index and TTL behaviour are server behaviour, and a fake would only test the
 * fake. The sandbox provides sb_pg and sb_redis under the target machine's
 * caps.
 */
#include <criterion/criterion.h>
#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "codegen.h"
#include "db/pg.h"
#include "observability/metrics.h"
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

/* The service reads only these fields, so a stack config keeps the test free of
 * the file and of the environment overrides config_load would apply. */
static void fill_config(config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->cache.redis_ttl_seconds = 3600;
    cfg->retention.default_ttl_days = 30;
    cfg->retention.max_ttl_days = 3650;
    cfg->codegen.min_length = CODE_MIN_LENGTH;
    cfg->codegen.max_length = CODE_MAX_LENGTH;
}

static PGconn *direct_conn(void)
{
    PGconn *c = PQconnectdb(test_dsn());
    cr_assert_not_null(c);
    cr_assert_eq(PQstatus(c), CONNECTION_OK, "connect failed: %s", PQerrorMessage(c));
    return c;
}

/* Deletes rows whose code starts with "test", so runs do not collide. Generated
 * codes are cleared by the test that makes them, by code. */
static void wipe_codes(void)
{
    PGconn *c = direct_conn();
    PGresult *r = PQexec(c, "DELETE FROM urls WHERE code LIKE 'test%'");
    cr_assert(PQresultStatus(r) == PGRES_COMMAND_OK, "wipe failed: %s",
              PQerrorMessage(c));
    PQclear(r);
    PQfinish(c);
}

/* An out-of-band delete, standing in for any other writer. The point is that
 * the cache does not know about it. */
static void delete_code(const char *code)
{
    PGconn *c = direct_conn();
    char sql[128];
    snprintf(sql, sizeof(sql), "DELETE FROM urls WHERE code = '%s'", code);
    PGresult *r = PQexec(c, sql);
    cr_assert(PQresultStatus(r) == PGRES_COMMAND_OK, "delete failed: %s",
              PQerrorMessage(c));
    PQclear(r);
    PQfinish(c);
}

static void forget(redis_client_t *cache, const char *code)
{
    char key[64];
    snprintf(key, sizeof(key), "url:%s", code);
    (void)redis_invalidate(cache, key);
}

Test(url_service, a_generated_code_is_the_configured_width_and_resolves)
{
    pg_pool_t *pool = fresh_pool();
    redis_client_t *cache = fresh_cache();
    config_t cfg;
    fill_config(&cfg);
    url_service_t *svc = url_service_new(pool, cache, &cfg, NULL);
    cr_assert_not_null(svc);

    char code[CODE_MAX_LENGTH + 1];
    cr_assert_eq(url_service_create(svc, "https://example.com/gen/1", NULL, 1,
                                    code, sizeof(code)),
                 URL_OK);
    cr_assert_eq(strlen(code), (size_t)CODE_MIN_LENGTH,
                 "a generated code must be %d characters, got '%s'",
                 CODE_MIN_LENGTH, code);

    char url[PG_MAX_URL + 1];
    cr_assert_eq(url_service_lookup(svc, code, url, sizeof(url), NULL), URL_OK);
    cr_assert_str_eq(url, "https://example.com/gen/1");

    /* The code is the id, so decoding it must give back the row id. That is the
     * invariant the schema comment claims and nothing else checks. */
    unsigned long long id = 0;
    cr_assert_eq(code_decode(code, &id), 0, "the generated code did not decode");
    PGconn *c = direct_conn();
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT id FROM urls WHERE code = '%s'", code);
    PGresult *r = PQexec(c, sql);
    cr_assert_eq(PQntuples(r), 1, "the row for '%s' is missing", code);
    cr_assert_eq(strtoull(PQgetvalue(r, 0, 0), NULL, 10), id,
                 "the code does not decode to the row id");
    PQclear(r);
    PQfinish(c);

    forget(cache, code);
    delete_code(code);
    redis_free(cache);
    url_service_free(svc);
    pg_pool_free(pool);
}

Test(url_service, a_client_chosen_code_is_stored_verbatim_and_resolves)
{
    pg_pool_t *pool = fresh_pool();
    redis_client_t *cache = fresh_cache();
    wipe_codes();
    config_t cfg;
    fill_config(&cfg);
    url_service_t *svc = url_service_new(pool, cache, &cfg, NULL);

    char code[CODE_MAX_LENGTH + 1];
    cr_assert_eq(url_service_create(svc, "https://example.com/custom", "testcust1",
                                    1, code, sizeof(code)),
                 URL_OK);
    cr_assert_str_eq(code, "testcust1");

    char url[PG_MAX_URL + 1];
    cr_assert_eq(url_service_lookup(svc, "testcust1", url, sizeof(url), NULL),
                 URL_OK);
    cr_assert_str_eq(url, "https://example.com/custom");

    forget(cache, "testcust1");
    wipe_codes();
    redis_free(cache);
    url_service_free(svc);
    pg_pool_free(pool);
}

Test(url_service, a_reused_custom_code_is_a_conflict)
{
    pg_pool_t *pool = fresh_pool();
    redis_client_t *cache = fresh_cache();
    wipe_codes();
    config_t cfg;
    fill_config(&cfg);
    url_service_t *svc = url_service_new(pool, cache, &cfg, NULL);

    char code[CODE_MAX_LENGTH + 1];
    cr_assert_eq(url_service_create(svc, "https://example.com/one", "testdup01", 1,
                                    code, sizeof(code)),
                 URL_OK);

    /* Same code, different target: the first URL must not be silently
     * overwritten, and the client must be told the code is taken. */
    cr_assert_eq(url_service_create(svc, "https://example.com/two", "testdup01", 1,
                                    code, sizeof(code)),
                 URL_CONFLICT);

    char url[PG_MAX_URL + 1];
    cr_assert_eq(url_service_lookup(svc, "testdup01", url, sizeof(url), NULL), URL_OK);
    cr_assert_str_eq(url, "https://example.com/one",
                     "the conflict overwrote the original URL");

    forget(cache, "testdup01");
    wipe_codes();
    redis_free(cache);
    url_service_free(svc);
    pg_pool_free(pool);
}

Test(url_service, an_unknown_code_is_not_found)
{
    pg_pool_t *pool = fresh_pool();
    redis_client_t *cache = fresh_cache();
    config_t cfg;
    fill_config(&cfg);
    url_service_t *svc = url_service_new(pool, cache, &cfg, NULL);

    forget(cache, "testnope1");
    char url[PG_MAX_URL + 1];
    cr_assert_eq(url_service_lookup(svc, "testnope1", url, sizeof(url), NULL),
                 URL_NOT_FOUND);

    redis_free(cache);
    url_service_free(svc);
    pg_pool_free(pool);
}

Test(url_service, a_malformed_code_is_invalid_rather_than_not_found)
{
    pg_pool_t *pool = fresh_pool();
    redis_client_t *cache = fresh_cache();
    config_t cfg;
    fill_config(&cfg);
    url_service_t *svc = url_service_new(pool, cache, &cfg, NULL);

    char url[PG_MAX_URL + 1];
    /* Too short, non-alphanumeric, and empty are client errors: 400, not 404.
     * Distinguishing them is the whole reason URL_INVALID exists. */
    cr_assert_eq(url_service_lookup(svc, "ab", url, sizeof(url), NULL), URL_INVALID);
    cr_assert_eq(url_service_lookup(svc, "has!bang", url, sizeof(url), NULL),
                 URL_INVALID);
    cr_assert_eq(url_service_lookup(svc, "", url, sizeof(url), NULL), URL_INVALID);

    redis_free(cache);
    url_service_free(svc);
    pg_pool_free(pool);
}

Test(url_service, an_expired_url_is_not_found_even_though_the_row_exists)
{
    pg_pool_t *pool = fresh_pool();
    redis_client_t *cache = fresh_cache();
    wipe_codes();
    config_t cfg;
    fill_config(&cfg);
    url_service_t *svc = url_service_new(pool, cache, &cfg, NULL);

    /* Inserted behind the service's back with an expiry in the past. */
    cr_assert_eq(pg_create_url(pool, "testexp01", "https://example.com/dead",
                               time(NULL) - 60),
                 PG_OK);
    forget(cache, "testexp01");

    char url[PG_MAX_URL + 1];
    cr_assert_eq(url_service_lookup(svc, "testexp01", url, sizeof(url), NULL),
                 URL_NOT_FOUND, "an expired URL was handed out");

    wipe_codes();
    redis_free(cache);
    url_service_free(svc);
    pg_pool_free(pool);
}

Test(url_service, a_deleted_url_survives_only_as_long_as_the_cache_does)
{
    pg_pool_t *pool = fresh_pool();
    redis_client_t *cache = fresh_cache();
    wipe_codes();
    config_t cfg;
    fill_config(&cfg);
    url_service_t *svc = url_service_new(pool, cache, &cfg, NULL);

    char code[CODE_MAX_LENGTH + 1];
    cr_assert_eq(url_service_create(svc, "https://example.com/gone", "testdel01", 1,
                                    code, sizeof(code)),
                 URL_OK);

    char url[PG_MAX_URL + 1];
    cr_assert_eq(url_service_lookup(svc, "testdel01", url, sizeof(url), NULL), URL_OK);

    /* Deleted out of band. The cache is a copy, so it keeps answering — this is
     * the divergence, stated rather than hidden. It is bounded by the TTL, and
     * anything that deletes a URL must invalidate the key. */
    delete_code("testdel01");
    cr_assert_eq(url_service_lookup(svc, "testdel01", url, sizeof(url), NULL), URL_OK,
                 "the cached copy should still answer until it is invalidated");

    forget(cache, "testdel01");
    cr_assert_eq(url_service_lookup(svc, "testdel01", url, sizeof(url), NULL),
                 URL_NOT_FOUND,
                 "after invalidation the database is the truth and the row is gone");

    wipe_codes();
    redis_free(cache);
    url_service_free(svc);
    pg_pool_free(pool);
}

Test(url_service, create_refuses_input_the_client_got_wrong)
{
    pg_pool_t *pool = fresh_pool();
    redis_client_t *cache = fresh_cache();
    config_t cfg;
    fill_config(&cfg);
    url_service_t *svc = url_service_new(pool, cache, &cfg, NULL);

    char code[CODE_MAX_LENGTH + 1];

    cr_assert_eq(url_service_create(svc, "", NULL, 1, code, sizeof(code)),
                 URL_INVALID, "an empty URL is not a URL");
    cr_assert_eq(url_service_create(svc, "https://example.com/x", "ab", 1,
                                    code, sizeof(code)),
                 URL_INVALID, "a 2-character custom code is below the minimum");
    cr_assert_eq(url_service_create(svc, "https://example.com/x", "has!bang", 1,
                                    code, sizeof(code)),
                 URL_INVALID, "a custom code must be alphanumeric");

    char too_small[CODE_MAX_LENGTH];
    cr_assert_eq(url_service_create(svc, "https://example.com/x", NULL, 1,
                                    too_small, sizeof(too_small)),
                 URL_INVALID, "a buffer too small for any code must be refused");

    /* One over the URL cap. */
    char big[PG_MAX_URL + 2];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    memcpy(big, "https://example.com/", 20);
    cr_assert_eq(url_service_create(svc, big, NULL, 1, code, sizeof(code)),
                 URL_INVALID);

    redis_free(cache);
    url_service_free(svc);
    pg_pool_free(pool);
}

Test(url_service, a_service_without_a_pool_is_refused)
{
    redis_client_t *cache = fresh_cache();
    config_t cfg;
    fill_config(&cfg);

    cr_assert_null(url_service_new(NULL, cache, &cfg, NULL));
    cr_assert_null(url_service_new(NULL, NULL, NULL, NULL));

    /* The cache, unlike the pool, is genuinely optional. */
    pg_pool_t *pool = fresh_pool();
    url_service_t *svc = url_service_new(pool, NULL, &cfg, NULL);
    cr_assert_not_null(svc);

    /* With no cache the service must still answer from the database. */
    char code[CODE_MAX_LENGTH + 1];
    cr_assert_eq(url_service_create(svc, "https://example.com/nocache",
                                    "testnoc1", 1, code, sizeof(code)),
                 URL_OK);
    char url[PG_MAX_URL + 1];
    cr_assert_eq(url_service_lookup(svc, "testnoc1", url, sizeof(url), NULL), URL_OK);
    cr_assert_str_eq(url, "https://example.com/nocache");

    wipe_codes();
    url_service_free(svc);
    pg_pool_free(pool);
    redis_free(cache);
}

Test(url_service, freeing_null_is_safe)
{
    url_service_free(NULL);
}

Test(url_service, cache_hits_and_misses_are_counted_separately)
{
    pg_pool_t *pool = fresh_pool();
    redis_client_t *cache = fresh_cache();
    config_t cfg;
    fill_config(&cfg);
    metrics_t *m = metrics_new();
    cr_assert_not_null(m);
    url_service_t *svc = url_service_new(pool, cache, &cfg, m);
    cr_assert_not_null(svc);

    /* Drop the entry create just cached, so the first lookup is a real miss. */
    char code[CODE_MAX_LENGTH + 1];
    cr_assert_eq(url_service_create(svc, "https://example.com/cold", NULL, 1, code,
                                    sizeof(code)),
                 URL_OK);
    char key[64];
    snprintf(key, sizeof(key), "url:%s", code);
    cr_assert_eq(redis_invalidate(cache, key), REDIS_OK);

    char url[PG_MAX_URL + 1];
    cr_assert_eq(url_service_lookup(svc, code, url, sizeof(url), NULL), URL_OK);
    cr_assert_eq(metrics_get(m, METRIC_CACHE_MISS_TOTAL), 1,
                 "a lookup the cache could not answer is a miss");
    cr_assert_eq(metrics_get(m, METRIC_CACHE_HIT_TOTAL), 0);

    /* The miss populated the cache, so the second lookup of the same code is a
     * hit. This is the distinction the load test needs: without it, a cold run
     * and a warm one are only told apart by their latency. */
    cr_assert_eq(url_service_lookup(svc, code, url, sizeof(url), NULL), URL_OK);
    cr_assert_eq(metrics_get(m, METRIC_CACHE_HIT_TOTAL), 1);
    cr_assert_eq(metrics_get(m, METRIC_CACHE_MISS_TOTAL), 1);

    /* A 404 counts as a miss: the cache was consulted and could not answer. */
    cr_assert_eq(url_service_lookup(svc, "nosuchcode", url, sizeof(url), NULL),
                 URL_NOT_FOUND);
    cr_assert_eq(metrics_get(m, METRIC_CACHE_MISS_TOTAL), 2);

    /* With no cache at all, neither counter moves: there was no cache to ask. */
    metrics_t *m2 = metrics_new();
    url_service_t *bare = url_service_new(pool, NULL, &cfg, m2);
    cr_assert_not_null(bare);
    cr_assert_eq(url_service_lookup(bare, code, url, sizeof(url), NULL), URL_OK);
    cr_assert_eq(metrics_get(m2, METRIC_CACHE_HIT_TOTAL), 0);
    cr_assert_eq(metrics_get(m2, METRIC_CACHE_MISS_TOTAL), 0);

    url_service_free(bare);
    metrics_free(m2);
    url_service_free(svc);
    metrics_free(m);
    redis_free(cache);
    pg_pool_free(pool);
}
