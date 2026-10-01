/* Redis cache: a single hiredis context used from the reactor's worker side.
 *
 * The cache is an optimisation, never a source of truth. Every failure here
 * means a miss, not an error the caller must handle specially: if Redis is gone
 * the service still answers from Postgres. That is why the miss and error codes
 * are distinguished but both lead to the same place in Task 7.
 */
#ifndef REDIS_H
#define REDIS_H

#include <stddef.h>

typedef struct redis_client redis_client_t;

#define REDIS_OK 0
#define REDIS_MISS 1
#define REDIS_ERROR (-1)

/* Connects to a redis:// URL. Returns NULL if the server cannot be reached or
 * does not answer PING. Caller frees with redis_free. */
redis_client_t *redis_init(const char *url);

/* Safe on NULL. */
void redis_free(redis_client_t *client);

/* GET key. Returns REDIS_OK and a NUL-terminated value in out, REDIS_MISS if
 * the key is absent, REDIS_ERROR on failure or if the value does not fit in
 * out_len. A value that does not fit is reported as an error rather than
 * truncated, because a truncated URL would redirect somewhere else. */
int redis_get(redis_client_t *client, const char *key, char *out, size_t out_len);

/* SETEX key ttl value. ttl is in seconds and must be positive: an entry with no
 * expiry is exactly the leak this cache must not have. Returns REDIS_OK or
 * REDIS_ERROR. */
int redis_setex(redis_client_t *client, const char *key, const char *value,
                 int ttl);

/* DEL key. Returns REDIS_OK whether or not the key existed. */
int redis_invalidate(redis_client_t *client, const char *key);

/* Remaining lifetime of key in seconds, or REDIS_MISS if absent or
 * persistent. Used by tests to prove entries actually carry a TTL. */
int redis_ttl(redis_client_t *client, const char *key);

#endif /* REDIS_H */