#include "services/url_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codegen.h"

/* Cache keys are namespaced so a URL entry can never collide with anything else
 * sharing the Redis database. */
#define CACHE_PREFIX "url:"
#define CACHE_KEY_MAX (sizeof(CACHE_PREFIX) - 1 + CODE_MAX_LENGTH + 1)

static void cache_key(char *out, size_t out_len, const char *code)
{
    snprintf(out, out_len, CACHE_PREFIX "%s", code);
}

struct url_service {
    pg_pool_t *pool;
    redis_client_t *cache; /* may be NULL */
    const config_t *cfg;
};

url_service_t *url_service_new(pg_pool_t *pool, redis_client_t *cache,
                               const config_t *cfg)
{
    /* A service without a database can do nothing; a service without a config
     * cannot decide expiries. The cache is genuinely optional. */
    if (!pool || !cfg)
        return NULL;

    url_service_t *svc = calloc(1, sizeof(*svc));
    if (!svc)
        return NULL;

    svc->pool = pool;
    svc->cache = cache;
    svc->cfg = cfg;
    return svc;
}

void url_service_free(url_service_t *svc)
{
    free(svc);
}

/* The expiry a request should get. A non-positive ttl asks for the configured
 * default; anything above the configured ceiling is capped, not refused,
 * because the ceiling is a retention budget and exceeding it is a request for
 * "as long as you keep things", not a malformed request. */
static time_t compute_expires(const config_t *cfg, int ttl_days, time_t now)
{
    int days = ttl_days;
    if (days <= 0)
        days = cfg->retention.default_ttl_days;
    if (days > cfg->retention.max_ttl_days)
        days = cfg->retention.max_ttl_days;
    if (days <= 0)
        return 0; /* configured to keep forever */
    return now + (time_t)days * 86400;
}

/* Best effort: the cache is an optimisation, so a failure here is dropped
 * rather than reported. The TTL never outlives the URL, which is what makes a
 * cache hit trustworthy without re-checking the database. */
static void cache_store(url_service_t *svc, const char *code, const char *url,
                        time_t expires_at, time_t now)
{
    if (!svc->cache)
        return;

    int ttl = svc->cfg->cache.redis_ttl_seconds;
    if (ttl <= 0)
        return;

    if (expires_at > 0) {
        time_t remaining = expires_at - now;
        if (remaining <= 0)
            return; /* already expired: caching it would serve a dead URL */
        if (remaining < (time_t)ttl)
            ttl = (int)remaining;
    }

    char key[CACHE_KEY_MAX];
    cache_key(key, sizeof(key), code);
    (void)redis_setex(svc->cache, key, url, ttl);
}

int url_service_create(url_service_t *svc, const char *url, const char *custom_code,
                       int ttl_days, char *out_code, size_t out_len)
{
    if (!svc || !url || !*url || !out_code)
        return URL_INVALID;
    if (strlen(url) > PG_MAX_URL)
        return URL_INVALID;
    /* The longest code plus its terminator must fit, or a generated code would
     * be silently truncated into one that resolves elsewhere. */
    if (out_len < CODE_MAX_LENGTH + 1)
        return URL_INVALID;

    time_t now = time(NULL);
    time_t expires_at = compute_expires(svc->cfg, ttl_days, now);

    if (custom_code && *custom_code) {
        /* Format first, so a malformed code is refused without a database round
         * trip. */
        if (!code_is_valid_custom(custom_code))
            return URL_INVALID;

        /* Then an existence check, before the insert, because the unique index
         * only covers (code, created_at): a code used in an earlier partition
         * would not collide, and two live rows with one code could not be told
         * apart by a lookup. */
        int exists = 0;
        if (pg_code_exists(svc->pool, custom_code, &exists) != PG_OK)
            return URL_ERROR;
        if (exists)
            return URL_CONFLICT;

        int rc = pg_create_url(svc->pool, custom_code, url, expires_at);
        if (rc == 1)
            return URL_CONFLICT; /* lost a race with a concurrent insert */
        if (rc != PG_OK)
            return URL_ERROR;

        snprintf(out_code, out_len, "%s", custom_code);
        cache_store(svc, custom_code, url, expires_at, now);
        return URL_OK;
    }

    /* Generated: the code is the base62 rendering of the id, so the id and the
     * code come from the same sequence value and cannot drift apart. */
    for (int attempt = 0; attempt < URL_GENERATE_ATTEMPTS; attempt++) {
        unsigned long long id = 0;
        if (pg_next_id(svc->pool, &id) != PG_OK)
            return URL_ERROR;

        char code[CODE_MAX_LENGTH + 1];
        if (code_encode(id, code, sizeof(code)) < 0)
            return URL_ERROR;

        int rc = pg_create_url_with_id(svc->pool, id, code, url, expires_at);
        if (rc == 1)
            continue; /* collided in this partition; take the next id */
        if (rc != PG_OK)
            return URL_ERROR;

        snprintf(out_code, out_len, "%s", code);
        cache_store(svc, code, url, expires_at, now);
        return URL_OK;
    }

    return URL_ERROR;
}

int url_service_lookup(url_service_t *svc, const char *code, char *out_url,
                       size_t out_len, time_t *out_expires_at)
{
    if (!svc || !code || !*code || !out_url || out_len == 0)
        return URL_INVALID;
    /* 4-12 alphanumeric covers both a client-chosen code and a generated one,
     * so anything else was never stored and cannot resolve. */
    if (!code_is_valid_custom(code))
        return URL_INVALID;

    if (out_expires_at)
        *out_expires_at = 0;

    if (svc->cache) {
        char key[CACHE_KEY_MAX];
        cache_key(key, sizeof(key), code);
        int rc = redis_get(svc->cache, key, out_url, out_len);
        if (rc == REDIS_OK)
            return URL_OK;
        /* Both MISS and ERROR fall through to the database. A broken cache
         * must look like a cold cache, never like a failure. */
    }

    time_t expires = 0;
    int rc = pg_lookup_url(svc->pool, code, out_url, out_len, &expires);
    if (rc == PG_NOT_FOUND)
        return URL_NOT_FOUND;
    if (rc != PG_OK)
        return URL_ERROR;

    if (out_expires_at)
        *out_expires_at = expires;
    cache_store(svc, code, out_url, expires, time(NULL));
    return URL_OK;
}
