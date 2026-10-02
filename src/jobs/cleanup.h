/* Retention: delete expired rows and drop them from the cache.
 *
 * This is a job, not a request path. It is meant to run from cron (or the
 * container orchestrator) against the same database the server uses, which is
 * why it takes a pool rather than a URL service: it needs the rows, not the
 * request semantics.
 *
 * The cache is a second copy of the data, so deleting the row is only half the
 * work. A cached entry keeps answering after its row is gone, and because the
 * redirect path never re-checks the database on a hit, nothing else would ever
 * notice. The callback exists so the row and the cache are removed by the same
 * job, in the same batch.
 */
#ifndef JOBS_CLEANUP_H
#define JOBS_CLEANUP_H

#include <time.h>

#include "db/pg.h"

/* 10k rows a batch: small enough that one statement does not hold locks on a
 * table that is still taking inserts for long, large enough that the round
 * trips are not the cost. */
#define CLEANUP_BATCH_SIZE 10000

/* Called once per deleted code, before the next batch is read. The code is valid
 * only for the call; a callback that needs to keep it must copy it. */
typedef void (*cleanup_invalidate_fn)(void *user_data, const char *code);

/* Deletes expired rows in batches until a batch comes back short, calling
 * invalidate for every code it removes so the cache cannot outlive the row.
 *
 * now is passed in rather than read here so a test can expire a row without
 * sleeping. batch_size <= 0 means CLEANUP_BATCH_SIZE. invalidate may be NULL.
 * out_deleted (optional) receives the total removed.
 *
 * Returns PG_OK, or PG_ERROR if a batch could not be deleted; a failed sweep
 * leaves the remaining rows for the next run, which is safe because deleting an
 * expired row is idempotent. */
int cleanup_run(pg_pool_t *pool, time_t now, int batch_size,
                cleanup_invalidate_fn invalidate, void *user_data,
                long *out_deleted);

#endif /* JOBS_CLEANUP_H */
