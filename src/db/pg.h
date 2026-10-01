/* Postgres access: a connection pool over libpq's blocking interface.
 *
 * The pool exists because the target is one core. A connection per request
 * would pay TCP and authentication setup on every lookup; a single shared
 * connection would serialise everything behind one socket buffer. A small pool
 * keeps both costs bounded and matches Postgres's own max_connections.
 *
 * Every function here blocks for the duration of its query. That is deliberate
 * for this layer: it keeps storage correctness independent of the reactor. The
 * reactor in Task 6 must not call these from its event loop, or one slow query
 * would stall every other connection — that integration belongs in the routing
 * layer, which submits work to worker threads.
 */
#ifndef PG_H
#define PG_H

#include <stddef.h>
#include <time.h>

typedef struct pg_pool pg_pool_t;

/* Result codes. The duplicate and not-found cases are separated from the error
 * case because the caller must answer 409 and 404 respectively; collapsing them
 * would make a failure look like a legitimate outcome. */
#define PG_OK 0
#define PG_NOT_FOUND 1
#define PG_ERROR (-1)

/* Longest target URL accepted. The HTTP body cap is larger, so this is the
 * tighter of the two limits and exists to keep one row's size predictable. */
#define PG_MAX_URL 2048

/* Opens pool_size connections. Returns NULL if the DSN is unusable or not one
 * connection could be opened. Caller frees with pg_pool_free. */
pg_pool_t *pg_pool_new(const char *dsn, int pool_size);

/* Safe on NULL. Closes every connection. */
void pg_pool_free(pg_pool_t *pool);

/* Applies schema.sql and creates the partitions covering the previous, current
 * and next month. Partitions are generated from the current date rather than
 * written into the file, because a date checked into a file silently rots and
 * the insert then fails with "no partition of relation found". */
int pg_run_schema(pg_pool_t *pool);

/* Inserts one URL. Returns PG_OK, PG_NOT_FOUND never, and:
 *   PG_ERROR   on any database failure, including a full disk
 *   1          when (code, created_at) collides, SQLSTATE 23505
 * A collision on a different created_at is NOT caught here: the unique index on
 * a partitioned table must include the partition key, so it enforces
 * (code, created_at) and not code alone. Keeping codes unique across partitions
 * is the caller's job, which is why pg_create_url accepts the code's owner to
 * check with. */
int pg_create_url(pg_pool_t *pool, const char *code, const char *url,
                  time_t expires_at);

/* Looks up a live URL. Returns PG_OK and fills out_url (NUL-terminated) and
 * out_expires_at, PG_NOT_FOUND for an unknown code or one whose expires_at has
 * passed, PG_ERROR on failure. A NULL expires_at is returned as 0, meaning no
 * expiry rather than 1970. */
int pg_lookup_url(pg_pool_t *pool, const char *code, char *out_url,
                  size_t out_len, time_t *out_expires_at);

/* Deletes every row whose expires_at has passed. Writes the count to
 * out_deleted. Returns PG_OK or PG_ERROR. */
int pg_cleanup_expired(pg_pool_t *pool, time_t now, long *out_deleted);

/* Number of live connections currently checked out, and the pool's size.
 * Exposed so tests can prove the pool bounds concurrency rather than merely
 * being configured to. */
int pg_pool_in_use(const pg_pool_t *pool);
int pg_pool_size(const pg_pool_t *pool);

#endif /* PG_H */