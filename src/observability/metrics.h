/* Process counters, rendered in Prometheus text format.
 *
 * A fixed set of counters, addressed by an enum. A free-form string key would
 * let a typo create a new series that reads zero forever, which is a silent
 * failure in exactly the place you look when something is wrong.
 *
 * Increments are plain, not atomic. The reactor is one thread; if requests are
 * ever handed to worker threads this becomes the first thing that must change,
 * and it is called out here so it is not discovered later.
 */
#ifndef OBSERVABILITY_METRICS_H
#define OBSERVABILITY_METRICS_H

#include "util/buf.h"

typedef struct metrics metrics_t;

typedef enum {
    METRIC_SHORTEN_TOTAL,        /* URLs created */
    METRIC_REDIRECT_TOTAL,       /* redirects served */
    METRIC_REDIRECT_MISS_TOTAL,  /* redirects for an unknown or expired code */
    METRIC_CACHE_HIT_TOTAL,      /* lookups answered from the cache */
    METRIC_CACHE_MISS_TOTAL,     /* lookups the cache could not answer */
    METRIC_RATE_LIMITED_TOTAL,   /* requests refused by the limiter */
    METRIC_CLIENT_ERROR_TOTAL,   /* responses with a 4xx status */
    METRIC_SERVER_ERROR_TOTAL,   /* responses with a 5xx status */
    METRIC_COUNT
} metric_id_t;

metrics_t *metrics_new(void);

/* Safe on NULL. */
void metrics_free(metrics_t *m);

/* Safe on NULL and on an out-of-range id, so a caller cannot corrupt memory by
 * miscounting an enum. */
void metrics_inc(metrics_t *m, metric_id_t id);
void metrics_add(metrics_t *m, metric_id_t id, unsigned long long n);
unsigned long long metrics_get(const metrics_t *m, metric_id_t id);

/* Appends every counter to out in the Prometheus text exposition format. */
void metrics_render(const metrics_t *m, buf_t *out);

#endif /* OBSERVABILITY_METRICS_H */
