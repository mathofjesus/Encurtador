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
 * is the caller's job; pg_code_exists is the tool for it. */
int pg_create_url(pg_pool_t *pool, const char *code, const char *url,
                  time_t expires_at);

/* Inserts one URL with a caller-assigned id, so a generated code can be the
 * base62 rendering of that id and the two never drift apart. The id must come
 * from pg_next_id; inventing one collides with the sequence's own future
 * values. Same return codes as pg_create_url. */
int pg_create_url_with_id(pg_pool_t *pool, unsigned long long id, const char *code,
                          const char *url, time_t expires_at);

/* Next value of urls_id_seq, which is the id a generated code encodes.
 * Returns PG_OK or PG_ERROR. */
int pg_next_id(pg_pool_t *pool, unsigned long long *out_id);

/* Whether any row, expired or not, already uses code. The caller uses this to
 * keep client-chosen codes unique across partitions, which the
 * (code, created_at) index cannot do on its own. Returns PG_OK or PG_ERROR. */
int pg_code_exists(pg_pool_t *pool, const char *code, int *out_exists);

/* Looks up a live URL. Returns PG_OK and fills out_url (NUL-terminated) and
 * out_expires_at, PG_NOT_FOUND for an unknown code or one whose expires_at has
 * passed, PG_ERROR on failure. A NULL expires_at is returned as 0, meaning no
 * expiry rather than 1970. */
int pg_lookup_url(pg_pool_t *pool, const char *code, char *out_url,
                  size_t out_len, time_t *out_expires_at);

/* Deletes up to limit expired rows in one statement and hands back their codes,
 * so the caller can invalidate each one in the cache.
 *
 * Batched rather than one unbounded DELETE: a single DELETE of every expired row
 * can hold a lock and a long transaction on a table that is still taking
 * inserts, and on this hardware the sweep competes with serving. A short
 * statement per batch keeps the interruption bounded.
 *
 * On PG_OK, *out_codes is a malloc'd array of *out_count NUL-terminated strings
 * (each malloc'd), or NULL when *out_count is 0. Free it with pg_free_codes.
 * Returns PG_OK or PG_ERROR. */
int pg_delete_expired_batch(pg_pool_t *pool, time_t now, int limit,
                            char ***out_codes, size_t *out_count);

/* Frees the array pg_delete_expired_batch returned. Safe on NULL. */
void pg_free_codes(char **codes, size_t count);

/* Number of live connections currently checked out, and the pool's size.
 * Exposed so tests can prove the pool bounds concurrency rather than merely
 * being configured to. */
int pg_pool_in_use(const pg_pool_t *pool);
int pg_pool_size(const pg_pool_t *pool);

#endif /* PG_H */