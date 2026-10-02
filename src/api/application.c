#include "api/application.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "api/router.h"
#include "cache/redis.h"
#include "db/pg.h"
#include "http/parser.h"
#include "jobs/cleanup.h"
#include "observability/metrics.h"
#include "services/rate_limit.h"
#include "services/url_service.h"
#include "util/buf.h"

/* At ~80 bytes per bucket this is about 320 KB, comfortably inside the app
 * container's 1 GB and enough distinct source addresses that a full table means
 * a flood, at which point the limiter evicts the least recently used key. */
#define RATE_LIMIT_MAX_BUCKETS 4096

struct application {
    /* Owned copy, so every component can point at one stable config. */
    config_t cfg;

    pg_pool_t *pool;
    redis_client_t *cache; /* may be NULL: cache disabled or unreachable */
    metrics_t *metrics;
    rl_t *rl;
    url_service_t *svc;
    router_t *router;

    log_t *log; /* borrowed, may be NULL */
};

static long long now_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void client_address(int fd, char *out, size_t out_len)
{
    if (!out || out_len == 0)
        return;
    out[0] = '\0';

    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &slen) != 0)
        return;

    void *addr = NULL;
    if (ss.ss_family == AF_INET)
        addr = &((struct sockaddr_in *)&ss)->sin_addr;
    else if (ss.ss_family == AF_INET6)
        addr = &((struct sockaddr_in6 *)&ss)->sin6_addr;

    if (addr)
        (void)inet_ntop(ss.ss_family, addr, out, (socklen_t)out_len);
}

/* The status line we just wrote is the only structured record of the outcome;
 * scanning our own output avoids threading a status out of every handler for
 * the sake of one log field. */
static int response_status(const char *response)
{
    int status = 0;
    if (response && sscanf(response, "HTTP/1.1 %d", &status) != 1)
        status = 0;
    return status;
}

application_t *application_new(const config_t *cfg, log_t *log)
{
    if (!cfg)
        return NULL;

    application_t *a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;

    a->cfg = *cfg;
    a->log = log;

    a->pool = pg_pool_new(a->cfg.database.primary_dsn, a->cfg.database.pool_size);
    if (!a->pool)
        goto fail;

    /* Idempotent, and cheaper than a migration tool for a schema that is a table
     * and its partitions. A failure here is fatal: without the table every
     * request is a 503 anyway, and failing loudly at startup beats serving
     * errors. */
    if (pg_run_schema(a->pool) != PG_OK)
        goto fail;

    a->cache = redis_init(a->cfg.cache.redis_url);
    if (!a->cache && a->log) {
        log_record_t rec = {0};
        rec.level = LOG_WARN;
        rec.event = "cache_unavailable";
        rec.error = "redis did not connect; every read will hit postgres";
        rec.duration_us = -1;
        log_write(a->log, &rec);
    }

    a->metrics = metrics_new();
    if (!a->metrics)
        goto fail;

    a->rl = rl_new(a->cfg.rate_limit.per_minute, a->cfg.rate_limit.burst,
                   RATE_LIMIT_MAX_BUCKETS);
    if (!a->rl)
        goto fail;

    a->svc = url_service_new(a->pool, a->cache, &a->cfg, a->metrics);
    if (!a->svc)
        goto fail;

    a->router = router_new(a->svc, a->rl, &a->cfg, a->metrics);
    if (!a->router)
        goto fail;

    return a;

fail:
    application_free(a);
    return NULL;
}

void application_free(application_t *a)
{
    if (!a)
        return;

    router_free(a->router);
    url_service_free(a->svc);
    rl_free(a->rl);
    metrics_free(a->metrics);
    redis_free(a->cache);
    pg_pool_free(a->pool);
    free(a);
}

int application_handle_conn(conn_t *c, void *user_data)
{
    application_t *a = user_data;
    if (!a || !c)
        return -1;

    buf_t *in = conn_input(c);
    if (!in || in->len == 0)
        return 0;

    http_request_t req;
    http_request_init(&req);
    int parsed = http_parse(&req, in->data, in->len);

    if (parsed == HTTP_PARSE_NEED_MORE)
        return 0; /* the rest is still on the wire */

    if (parsed == HTTP_PARSE_MALFORMED) {
        /* Answer and close. There is no way to know where the next request would
         * start in a stream we could not parse, so keeping the connection would
         * mean guessing. */
        if (a->metrics)
            metrics_inc(a->metrics, METRIC_CLIENT_ERROR_TOTAL);

        buf_t *out = buf_new(256);
        if (out) {
            http_response_write(out, 400, "application/json",
                                "{\"error\":\"bad request\"}\n", NULL, NULL, 0, 0);
            (void)conn_write(c, out->data, out->len);
            buf_free(out);
        }
        conn_consume(c, in->len);
        return -1;
    }

    char client[64];
    client_address(c->fd, client, sizeof(client));

    long long t0 = now_us();

    buf_t *out = buf_new(1024);
    if (!out) {
        conn_consume(c, req.consumed);
        return -1;
    }

    int rc = router_dispatch(a->router, &req, client, out);
    long long duration = now_us() - t0;

    if (conn_write(c, out->data, out->len) != 0)
        rc = -1;

    if (a->log) {
        log_record_t rec = {0};
        rec.level = LOG_INFO;
        rec.event = "request";
        rec.method = req.method;
        rec.path = req.path;
        rec.status = response_status(out->data);
        rec.duration_us = duration;
        rec.client = client[0] ? client : NULL;
        log_write(a->log, &rec);
    }

    buf_free(out);
    conn_consume(c, req.consumed);
    return rc;
}

/* The cache invalidation the cleanup job performs per deleted code. It goes
 * through the URL service so the key format stays in one place. */
static void invalidate_code(void *user_data, const char *code)
{
    url_service_invalidate((url_service_t *)user_data, code);
}

int application_cleanup(application_t *a, time_t now, int batch_size,
                        long *out_deleted)
{
    if (!a)
        return PG_ERROR;

    return cleanup_run(a->pool, now, batch_size, invalidate_code, a->svc,
                       out_deleted);
}
