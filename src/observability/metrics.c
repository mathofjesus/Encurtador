#include "observability/metrics.h"

#include <stdlib.h>

struct metrics {
    unsigned long long counters[METRIC_COUNT];
};

/* Names and help strings live in one table next to the enum, so adding a
 * counter is a two-line change and the renderer cannot drift from the enum. */
static const struct {
    const char *name;
    const char *help;
} defs[METRIC_COUNT] = {
    [METRIC_SHORTEN_TOTAL] =
        {"shortener_shorten_total", "URLs created"},
    [METRIC_REDIRECT_TOTAL] =
        {"shortener_redirect_total", "Redirects served"},
    [METRIC_REDIRECT_MISS_TOTAL] =
        {"shortener_redirect_miss_total", "Redirects for an unknown or expired code"},
    [METRIC_RATE_LIMITED_TOTAL] =
        {"shortener_rate_limited_total", "Requests refused by the rate limiter"},
    [METRIC_CLIENT_ERROR_TOTAL] =
        {"shortener_client_error_total", "Responses with a 4xx status"},
    [METRIC_SERVER_ERROR_TOTAL] =
        {"shortener_server_error_total", "Responses with a 5xx status"},
};

static int id_valid(metric_id_t id)
{
    /* Cast to unsigned so a negative id is rejected without a comparison that
     * the compiler can prove is always false. */
    return (unsigned)id < (unsigned)METRIC_COUNT;
}

metrics_t *metrics_new(void)
{
    return calloc(1, sizeof(metrics_t));
}

void metrics_free(metrics_t *m)
{
    free(m);
}

void metrics_add(metrics_t *m, metric_id_t id, unsigned long long n)
{
    if (!m || !id_valid(id))
        return;
    m->counters[id] += n;
}

void metrics_inc(metrics_t *m, metric_id_t id)
{
    metrics_add(m, id, 1);
}

unsigned long long metrics_get(const metrics_t *m, metric_id_t id)
{
    if (!m || !id_valid(id))
        return 0;
    return m->counters[id];
}

void metrics_render(const metrics_t *m, buf_t *out)
{
    if (!out)
        return;

    for (int i = 0; i < METRIC_COUNT; i++) {
        /* An explicit zero series is emitted even with no traffic. A series
         * that only appears once it is non-zero makes a dashboard show "no
         * data" in exactly the calm case it should show 0. */
        buf_appendf(out, "# HELP %s %s\n", defs[i].name, defs[i].help);
        buf_appendf(out, "# TYPE %s counter\n", defs[i].name);
        buf_appendf(out, "%s %llu\n", defs[i].name,
                    metrics_get(m, (metric_id_t)i));
    }
}
