#include "jobs/cleanup.h"

int cleanup_run(pg_pool_t *pool, time_t now, int batch_size,
                cleanup_invalidate_fn invalidate, void *user_data,
                long *out_deleted)
{
    if (!pool)
        return PG_ERROR;

    if (batch_size <= 0)
        batch_size = CLEANUP_BATCH_SIZE;

    long total = 0;
    for (;;) {
        char **codes = NULL;
        size_t count = 0;

        int rc = pg_delete_expired_batch(pool, now, batch_size, &codes, &count);
        if (rc != PG_OK) {
            /* The rows in this batch are already gone; a retry would find them
             * absent and move on, which is why the failure is reported rather
             * than retried in a loop here. */
            pg_free_codes(codes, count);
            return rc;
        }

        for (size_t i = 0; i < count; i++) {
            if (invalidate)
                invalidate(user_data, codes[i]);
        }
        total += (long)count;
        pg_free_codes(codes, count);

        /* A short batch means there was nothing left to fill it, so the sweep is
         * done. Asking again after an exactly-full batch costs one empty query
         * and is what makes the full-batch case correct. */
        if (count < (size_t)batch_size)
            break;
    }

    if (out_deleted)
        *out_deleted = total;
    return PG_OK;
}
