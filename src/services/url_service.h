/* URL shortener service: the rules that sit between HTTP and storage.
 *
 * Everything here is synchronous. It calls libpq and hiredis on the calling
 * thread, which keeps the storage logic independent of the reactor. On one core
 * that means a request waits for its database round trip; hiding that latency
 * behind worker threads is a later concern and does not change these rules.
 *
 * The cache is a read-through optimisation and never a source of truth. Its
 * entries are given a TTL no longer than the URL's own remaining lifetime, so a
 * cache hit can never serve a URL the database has already expired.
 */
#ifndef SERVICES_URL_SERVICE_H
#define SERVICES_URL_SERVICE_H

#include <stddef.h>
#include <time.h>

#include "cache/redis.h"
#include "config.h"
#include "db/pg.h"

typedef struct url_service url_service_t;

/* Result codes. URL_INVALID is separate from URL_ERROR on purpose: a client
 * mistake must become 400 while a database failure must become 503, and
 * collapsing them would tell a client its input was bad when the truth is that
 * storage is down. */
#define URL_OK 0
#define URL_NOT_FOUND 1
#define URL_CONFLICT 2
#define URL_INVALID 3
#define URL_ERROR (-1)

/* How many sequence values to try before giving up on a generated code. A
 * collision needs two live rows with the same code in one partition, which the
 * sequence makes impossible in practice; the loop exists so the impossibility is
 * written down and handled rather than assumed. */
#define URL_GENERATE_ATTEMPTS 5

/* cache may be NULL (cache disabled or unavailable); pool and cfg are required.
 * The service does not own any of them. Returns NULL on invalid arguments. */
url_service_t *url_service_new(pg_pool_t *pool, redis_client_t *cache,
                               const config_t *cfg);

/* Safe on NULL. Does not free the pool, cache or config. */
void url_service_free(url_service_t *svc);

/* Creates a short URL and writes the code to out_code.
 *
 * custom_code NULL or empty asks for a generated code; otherwise it must be a
 * 4-12 character alphanumeric code chosen by the client. ttl_days <= 0 selects
 * the configured default, and a value above the configured maximum is capped
 * there rather than refused.
 *
 * Returns URL_OK, URL_CONFLICT for a custom code already in use, URL_INVALID for
 * bad input, or URL_ERROR. out_code is only written on URL_OK. */
int url_service_create(url_service_t *svc, const char *url, const char *custom_code,
                       int ttl_days, char *out_code, size_t out_len);

/* Resolves code to its target URL. On URL_OK, out_url is NUL-terminated and
 * out_expires_at is the expiry (0 when none, or when the hit came from the
 * cache, which only ever stores URLs that are still live).
 *
 * Returns URL_OK, URL_NOT_FOUND, URL_INVALID for a malformed code, or
 * URL_ERROR. */
int url_service_lookup(url_service_t *svc, const char *code, char *out_url,
                       size_t out_len, time_t *out_expires_at);

/* Drops code from the cache. The cleanup job calls this after deleting an
 * expired row, so a cache entry that outlived its row cannot keep answering for
 * it. Safe on NULL svc, a NULL cache, and a NULL/empty code. */
void url_service_invalidate(url_service_t *svc, const char *code);

#endif /* SERVICES_URL_SERVICE_H */
